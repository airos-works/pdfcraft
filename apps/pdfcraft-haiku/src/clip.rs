//! The clipboard's file formats as text. Platform-neutral.
//!
//! Files copied in Tracker are entry_refs; programs ported to air/OS (and browsers) copy files
//! and links as `text/uri-list` (RFC 2483: one URI per line, CRLF, `#` comments). Where the
//! interface takes clipboard text (a text field, Create ▸ Clipboard), files paste as their
//! paths and links as the links, as they do from a file manager on Linux.

/// `text/uri-list` for files: `file://` URIs, CRLF after each.
pub fn uri_list(paths: &[std::path::PathBuf]) -> String {
    use std::os::unix::ffi::OsStrExt;
    let mut out = String::new();
    for path in paths {
        out.push_str("file://");
        for &b in path.as_os_str().as_bytes() {
            if b.is_ascii_alphanumeric() || b"/-_.~".contains(&b) {
                out.push(char::from(b));
            } else {
                out.push_str(&format!("%{b:02X}"));
            }
        }
        out.push_str("\r\n");
    }
    out
}

/// A `text/uri-list` as text: one line per entry, files as their paths and other URIs as they
/// are. Comments and blank lines go.
pub fn uri_list_text(list: &str) -> String {
    let lines: Vec<String> = list
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(|uri| file_path(uri).unwrap_or_else(|| uri.to_owned()))
        .collect();
    lines.join("\n")
}

/// The path of a local `file:` URI (`file:///p`, `file://localhost/p`, `file:/p`), decoded.
fn file_path(uri: &str) -> Option<String> {
    let rest = uri.strip_prefix("file:")?;
    let rest = rest.strip_prefix("//localhost").or_else(|| rest.strip_prefix("//")).unwrap_or(rest);
    if !rest.starts_with('/') {
        return None;
    }
    let bytes = rest.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while let Some(&b) = bytes.get(i) {
        let hex = bytes.get(i + 1..i + 3).and_then(|h| std::str::from_utf8(h).ok()).and_then(|h| u8::from_str_radix(h, 16).ok());
        match (b, hex) {
            (b'%', Some(v)) => {
                out.push(v);
                i += 3;
            }
            _ => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8(out).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn files_go_out_as_file_uris_and_come_back_as_paths() {
        let paths = vec![std::path::PathBuf::from("/boot/home/My Files/a%b é.pdf"), std::path::PathBuf::from("/tmp/x.png")];
        let list = uri_list(&paths);
        assert_eq!(list, "file:///boot/home/My%20Files/a%25b%20%C3%A9.pdf\r\nfile:///tmp/x.png\r\n");
        assert_eq!(uri_list_text(&list), "/boot/home/My Files/a%b é.pdf\n/tmp/x.png");
    }

    #[test]
    fn links_stay_links_and_comments_go() {
        let list = "# copied\r\nhttps://example.com/a%20b\r\n\r\nfile://localhost/boot/home/x.pdf\r\nfile:/tmp/y.pdf\nfile://server/share/z.pdf\n";
        assert_eq!(uri_list_text(list), "https://example.com/a%20b\n/boot/home/x.pdf\n/tmp/y.pdf\nfile://server/share/z.pdf");
    }

    #[test]
    fn broken_escapes_and_bad_utf8_dont_panic() {
        assert_eq!(uri_list_text("file:///a%zz%4"), "/a%zz%4");
        assert_eq!(uri_list_text("file:///%ff"), "file:///%ff");
        assert_eq!(uri_list_text("file:///%"), "/%");
        assert_eq!(uri_list_text(""), "");
    }
}
