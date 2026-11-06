use std::path::{Path, PathBuf};

use pptx_render::{fit_scale, render_deck, Options};

fn fixture(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures")
        .join(name)
}

fn temp(name: &str) -> PathBuf {
    let base = std::env::temp_dir().join(format!(
        "pptx-render-test-{}-{}",
        std::process::id(),
        name
    ));
    let _ = std::fs::remove_dir_all(&base);
    base
}

#[test]
fn scale_fits_each_aspect_inside_the_canvas() {
    assert_eq!(fit_scale(1280.0, 720.0, 1920, 1080).unwrap(), 1.5);
    assert_eq!(fit_scale(960.0, 720.0, 1920, 1080).unwrap(), 1.5);
    let wide = fit_scale(1280.0, 800.0, 1920, 1080).unwrap();
    assert!((wide - 1.35).abs() < 1e-6, "{wide}");
    assert!(fit_scale(0.0, 720.0, 1920, 1080).is_err());
}

#[test]
fn renders_16x9_at_1080p_and_skips_hidden_slides() {
    let out = temp("16x9");
    let manifest = render_deck(&fixture("16x9.pptx"), &out, &Options::default()).unwrap();
    assert_eq!(manifest.pages.len(), 2);
    assert_eq!(manifest.hidden_pages, vec![3]);
    for page in &manifest.pages {
        assert_eq!((page.width, page.height), (1920, 1080));
        assert!(out.join(&page.file).exists(), "{}", page.file);
    }
    assert!(out.join("manifest.json").exists());
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn renders_4x3_without_cropping() {
    let out = temp("4x3");
    let manifest = render_deck(
        &fixture("4x3.pptx"),
        &out,
        &Options {
            width: 1920,
            height: 1080,
            include_hidden: false,
        },
    )
    .unwrap();
    assert_eq!(manifest.pages.len(), 1);
    assert_eq!((manifest.pages[0].width, manifest.pages[0].height), (1440, 1080));
    let _ = std::fs::remove_dir_all(&out);
}

#[test]
fn rendering_is_deterministic() {
    let first = temp("deterministic-a");
    let second = temp("deterministic-b");
    let options = Options::default();
    render_deck(&fixture("16x9.pptx"), &first, &options).unwrap();
    render_deck(&fixture("16x9.pptx"), &second, &options).unwrap();
    for page in 1..=2 {
        let name = format!("slide-{page:02}.png");
        assert_eq!(
            std::fs::read(first.join(&name)).unwrap(),
            std::fs::read(second.join(&name)).unwrap()
        );
    }
    let _ = std::fs::remove_dir_all(&first);
    let _ = std::fs::remove_dir_all(&second);
}

#[test]
fn corrupt_input_writes_nothing() {
    let input = temp("corrupt-input");
    std::fs::create_dir_all(&input).unwrap();
    let deck = input.join("broken.pptx");
    std::fs::write(&deck, b"not a zip package").unwrap();
    let out = temp("corrupt-out");
    assert!(render_deck(&deck, &out, &Options::default()).is_err());
    assert!(!out.exists(), "a failed render must not leave a directory");
    let _ = std::fs::remove_dir_all(&input);
}

#[test]
fn unicode_paths_work() {
    let root = temp("unicode-礼物");
    std::fs::create_dir_all(&root).unwrap();
    let deck = root.join("演示.pptx");
    std::fs::copy(fixture("16x9.pptx"), &deck).unwrap();
    let out = root.join("输出");
    let manifest = render_deck(&deck, &out, &Options::default()).unwrap();
    assert_eq!(manifest.pages.len(), 2);
    let _ = std::fs::remove_dir_all(&root);
}
