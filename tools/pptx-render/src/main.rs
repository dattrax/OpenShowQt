//! CLI entry point: `pptx-render --input deck.pptx --out-dir DIR [--width N] [--height N]`.

use std::path::PathBuf;
use std::process::ExitCode;

use pptx_render::{render_deck, Manifest, Options};

fn main() -> ExitCode {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(real_main)) {
        Ok(Ok(())) => ExitCode::SUCCESS,
        Ok(Err(error)) => {
            eprintln!("pptx-render: {error}");
            ExitCode::from(2)
        }
        Err(_) => {
            eprintln!("pptx-render: the renderer panicked; nothing was imported");
            ExitCode::from(3)
        }
    }
}

fn real_main() -> Result<(), String> {
    let mut input: Option<PathBuf> = None;
    let mut out_dir: Option<PathBuf> = None;
    let mut options = Options::default();
    let mut args = std::env::args_os().skip(1);
    while let Some(arg) = args.next() {
        match arg.to_str() {
            Some("--input") => input = Some(next_path(&mut args, "--input")?),
            Some("--out-dir") => out_dir = Some(next_path(&mut args, "--out-dir")?),
            Some("--width") => options.width = next_number(&mut args, "--width")?,
            Some("--height") => options.height = next_number(&mut args, "--height")?,
            Some("--include-hidden") => options.include_hidden = true,
            Some("--help") | Some("-h") => {
                usage();
                return Ok(());
            }
            Some(other) => return Err(format!("unknown argument '{other}' (try --help)")),
            None => return Err("arguments must be valid Unicode (try --help)".to_string()),
        }
    }
    let input = input.ok_or_else(|| "--input is required (try --help)".to_string())?;
    let out_dir = out_dir.ok_or_else(|| "--out-dir is required (try --help)".to_string())?;
    let manifest: Manifest = render_deck(&input, &out_dir, &options)?;
    println!(
        "pptx-render: {} visible page(s) at {}x{} -> {}",
        manifest.pages.len(),
        manifest.pages.first().map(|page| page.width).unwrap_or(0),
        manifest.pages.first().map(|page| page.height).unwrap_or(0),
        out_dir.display()
    );
    Ok(())
}

fn next_path(args: &mut impl Iterator<Item = std::ffi::OsString>, flag: &str) -> Result<PathBuf, String> {
    args.next()
        .map(PathBuf::from)
        .ok_or_else(|| format!("{flag} needs a path"))
}

fn next_number(args: &mut impl Iterator<Item = std::ffi::OsString>, flag: &str) -> Result<u32, String> {
    let raw = args
        .next()
        .ok_or_else(|| format!("{flag} needs a number"))?;
    let text = raw
        .to_str()
        .ok_or_else(|| format!("{flag} needs a number"))?;
    text.parse::<u32>()
        .map_err(|_| format!("{flag} needs a positive whole number, found '{text}'"))
}

fn usage() {
    println!(
        "Usage: {} --input DECK.pptx --out-dir DIR [--width 1920] [--height 1080] [--include-hidden]",
        pptx_render::binary_name()
    );
}
