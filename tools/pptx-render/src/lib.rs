//! Render a `.pptx` to one PNG per visible slide.
//!
//! The renderer is BetterOffice's native Rust engine. This crate is a thin,
//! deterministic wrapper: it decides the pixel scale from the deck's own
//! display list, skips hidden slides, writes zero-padded page images and a
//! JSON manifest, and leaves every pixel decision to the engine.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use betteroffice_pptx::{Background, PngTarget, Presentation, RenderOptions};
use serde::Serialize;

/// The authored canvas OpenShowQt presents: the deck is fitted inside this
/// box without cropping. A 16:9 deck reaches exactly 1920x1080; 4:3 reaches
/// 1440x1080; 16:10 reaches 1728x1080.
pub const DEFAULT_WIDTH: u32 = 1920;
pub const DEFAULT_HEIGHT: u32 = 1080;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Options {
    pub width: u32,
    pub height: u32,
    pub include_hidden: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            width: DEFAULT_WIDTH,
            height: DEFAULT_HEIGHT,
            include_hidden: false,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Manifest {
    pub format: u32,
    pub renderer: String,
    pub source: String,
    pub source_bytes: u64,
    pub target_width: u32,
    pub target_height: u32,
    pub scale: f32,
    pub slide_width: f32,
    pub slide_height: f32,
    pub pages: Vec<Page>,
    /// 1-based source page numbers that were hidden and therefore not written.
    pub hidden_pages: Vec<usize>,
    /// Families the deck asked for that the bundled faces did not match.
    pub font_substitutions: Vec<FontSubstitution>,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Page {
    /// 1-based ordinal in the source deck.
    pub page: usize,
    pub file: String,
    pub width: u32,
    pub height: u32,
    pub skipped_images: usize,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FontSubstitution {
    pub requested: String,
    pub selected: String,
    /// 1-based source page where the substitution first appeared.
    pub first_page: usize,
}

/// Render `input` into `out_dir` and return the manifest that was written.
///
/// The output directory must not already contain a deck render; the caller
/// owns cache-directory selection. All slides are rendered in memory before
/// anything is written, so a failure leaves no partial page behind.
pub fn render_deck(input: &Path, out_dir: &Path, options: &Options) -> Result<Manifest, String> {
    if options.width == 0 || options.height == 0 {
        return Err("target width and height must be positive".to_string());
    }
    let bytes = std::fs::read(input)
        .map_err(|error| format!("reading '{}': {error}", input.display()))?;
    let presentation = Presentation::open(&bytes)
        .map_err(|error| format!("opening '{}': {error}", input.display()))?;

    let slide_count = presentation.slides().len();
    if slide_count == 0 {
        return Err(format!("'{}' contains no slides", input.display()));
    }

    let visible: Vec<usize> = (0..slide_count)
        .filter(|index| {
            options.include_hidden || !presentation.slides()[*index].hidden.unwrap_or(false)
        })
        .collect();
    if visible.is_empty() {
        return Err(format!(
            "'{}' contains only hidden slides; nothing to import",
            input.display()
        ));
    }

    // Every slide shares the deck's surface. Lay the first visible slide out
    // once to learn that surface, then scale it into the target box.
    let first = presentation
        .render_slide(visible[0])
        .map_err(|error| format!("laying out page {}: {error}", visible[0] + 1))?;
    let slide_width = first.display_list.width;
    let slide_height = first.display_list.height;
    let scale = fit_scale(slide_width, slide_height, options.width, options.height)?;

    let digits = visible.len().to_string().len().max(2);
    let mut pages = Vec::with_capacity(visible.len());
    let mut hidden_pages = Vec::new();
    let mut substitutions: BTreeSet<(String, String, usize)> = BTreeSet::new();
    let mut rendered: Vec<(String, Vec<u8>, Page)> = Vec::with_capacity(visible.len());

    for index in 0..slide_count {
        let page = index + 1;
        if !visible.contains(&index) {
            hidden_pages.push(page);
            continue;
        }
        let laid_out = presentation
            .render_slide(index)
            .map_err(|error| format!("laying out page {page}: {error}"))?;
        for substitution in &laid_out.font_substitutions {
            substitutions.insert((
                substitution.requested_family.clone(),
                substitution.selected_family.clone(),
                page,
            ));
        }
        let png = presentation
            .render_png(
                PngTarget { slide: index },
                &RenderOptions {
                    scale,
                    background: Background::Slide,
                    ..RenderOptions::default()
                },
            )
            .map_err(|error| format!("rendering page {page}: {error}"))?;
        let file = format!("slide-{page:0digits$}.png");
        let entry = Page {
            page,
            file: file.clone(),
            width: png.width,
            height: png.height,
            skipped_images: png.skipped_images,
        };
        rendered.push((file, png.bytes, entry));
    }

    if rendered.is_empty() {
        return Err(format!("'{}' produced no visible pages", input.display()));
    }
    if out_dir.exists() {
        let empty = std::fs::read_dir(out_dir)
            .map_err(|error| format!("reading output directory '{}': {error}", out_dir.display()))?
            .next()
            .is_none();
        if !empty {
            return Err(format!(
                "output directory '{}' already contains files",
                out_dir.display()
            ));
        }
    }

    std::fs::create_dir_all(out_dir)
        .map_err(|error| format!("creating '{}': {error}", out_dir.display()))?;
    for (file, bytes, entry) in &rendered {
        let path = out_dir.join(file);
        if let Err(error) = std::fs::write(&path, bytes) {
            let _ = std::fs::remove_dir_all(out_dir);
            return Err(format!("writing '{}': {error}", path.display()));
        }
        pages.push(entry.clone());
    }

    // BTreeSet sorted by (requested, selected, page); keep the first page for
    // each family pair.
    let mut fonts: Vec<FontSubstitution> = Vec::new();
    for (requested, selected, page) in substitutions {
        if let Some(existing) = fonts
            .iter_mut()
            .find(|entry| entry.requested == requested && entry.selected == selected)
        {
            existing.first_page = existing.first_page.min(page);
        } else {
            fonts.push(FontSubstitution {
                requested,
                selected,
                first_page: page,
            });
        }
    }

    let manifest = Manifest {
        format: 1,
        renderer: "betteroffice-0.5.1".to_string(),
        source: input
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| input.display().to_string()),
        source_bytes: bytes.len() as u64,
        target_width: options.width,
        target_height: options.height,
        scale,
        slide_width,
        slide_height,
        pages,
        hidden_pages,
        font_substitutions: fonts,
    };
    let path = out_dir.join("manifest.json");
    let json = serde_json::to_vec_pretty(&manifest)
        .map_err(|error| format!("encoding '{}': {error}", path.display()))?;
    if let Err(error) = std::fs::write(&path, json) {
        let _ = std::fs::remove_dir_all(out_dir);
        return Err(format!("writing '{}': {error}", path.display()));
    }
    Ok(manifest)
}

/// Scale that fits `width` x `height` inside the target box, preserving aspect.
pub fn fit_scale(width: f32, height: f32, target_width: u32, target_height: u32) -> Result<f32, String> {
    if !width.is_finite() || !height.is_finite() || width <= 0.0 || height <= 0.0 {
        return Err(format!("slide surface {width}x{height} is not a usable size"));
    }
    let scale = (target_width as f32 / width).min(target_height as f32 / height);
    if !scale.is_finite() || scale <= 0.0 {
        return Err("computed render scale is not positive".to_string());
    }
    Ok(scale)
}

/// The binary path a build produces, for tests and the frontend.
pub fn binary_name() -> &'static str {
    if cfg!(windows) { "pptx-render.exe" } else { "pptx-render" }
}

/// Convenience for callers that hold only a path string.
pub fn render_deck_to(input: PathBuf, out_dir: PathBuf, options: &Options) -> Result<Manifest, String> {
    render_deck(&input, &out_dir, options)
}
