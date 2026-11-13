//! A line-preserving editor for the emulator's `qt-config.ini` (SimpleIni, `key=value`, section
//! and key names case-insensitive). Only the keys the launcher sets change; every other line,
//! comment and blank line stays as it was.

pub struct IniDoc {
    lines: Vec<String>,
    crlf: bool,
    bom: bool,
}

fn section_name(line: &str) -> Option<&str> {
    let t = line.trim();
    (t.starts_with('[') && t.ends_with(']') && t.len() >= 2).then(|| t[1..t.len() - 1].trim())
}

fn key_of(line: &str) -> Option<&str> {
    let t = line.trim_start();
    if t.is_empty() || t.starts_with(';') || t.starts_with('#') || t.starts_with('[') {
        return None;
    }
    let eq = t.find('=')?;
    Some(t[..eq].trim())
}

impl IniDoc {
    pub fn parse(text: &str) -> Self {
        let bom = text.starts_with('\u{feff}');
        let body = text.trim_start_matches('\u{feff}');
        let crlf = body.contains("\r\n") || !body.contains('\n');
        let mut lines: Vec<String> = body.split('\n').map(|l| l.trim_end_matches('\r').to_string()).collect();
        // A trailing newline leaves one empty element; it is written back by `render`.
        if lines.last().map(|l| l.is_empty()).unwrap_or(false) {
            lines.pop();
        }
        Self { lines, crlf, bom }
    }

    /// Line range `[header, end)` of `section`, the header included.
    fn section_range(&self, section: &str) -> Option<(usize, usize)> {
        let start = self
            .lines
            .iter()
            .position(|l| section_name(l).map(|s| s.eq_ignore_ascii_case(section)).unwrap_or(false))?;
        let end = self.lines[start + 1..]
            .iter()
            .position(|l| section_name(l).is_some())
            .map(|i| start + 1 + i)
            .unwrap_or(self.lines.len());
        Some((start, end))
    }

    pub fn get(&self, section: &str, key: &str) -> Option<String> {
        let (start, end) = self.section_range(section)?;
        self.lines[start + 1..end].iter().find_map(|l| {
            let k = key_of(l)?;
            if k.eq_ignore_ascii_case(key) {
                let eq = l.find('=')?;
                Some(l[eq + 1..].trim().to_string())
            } else {
                None
            }
        })
    }

    /// Sets `key=value` in `section`, adding the key (or the section) when missing.
    /// Returns whether the file changed.
    pub fn set(&mut self, section: &str, key: &str, value: &str) -> bool {
        let new_line = format!("{key}={value}");
        let Some((start, end)) = self.section_range(section) else {
            if self.lines.last().map(|l| !l.trim().is_empty()).unwrap_or(false) {
                self.lines.push(String::new());
            }
            self.lines.push(format!("[{section}]"));
            self.lines.push(new_line);
            return true;
        };
        let mut changed = false;
        let mut found = false;
        for line in &mut self.lines[start + 1..end] {
            if key_of(line).map(|k| k.eq_ignore_ascii_case(key)).unwrap_or(false) {
                found = true;
                if *line != new_line {
                    *line = new_line.clone();
                    changed = true;
                }
            }
        }
        if !found {
            // After the section's last non-blank line, so the blank line before the next
            // section stays where it was.
            let mut at = end;
            while at > start + 1 && self.lines[at - 1].trim().is_empty() {
                at -= 1;
            }
            self.lines.insert(at, new_line);
            changed = true;
        }
        changed
    }

    pub fn render(&self) -> String {
        let nl = if self.crlf { "\r\n" } else { "\n" };
        let mut out = String::new();
        if self.bom {
            out.push('\u{feff}');
        }
        for line in &self.lines {
            out.push_str(line);
            out.push_str(nl);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "[Renderer]\r\nbackend\\default=true\r\nbackend=1\r\nfps_limit\\default=true\r\nfps_limit=0\r\n\r\n[System]\r\nuse_docked_mode=0\r\n";

    #[test]
    fn replaces_in_place_and_keeps_the_rest() {
        let mut doc = IniDoc::parse(SAMPLE);
        assert!(doc.set("Renderer", "fps_limit\\default", "false"));
        assert!(doc.set("Renderer", "fps_limit", "30"));
        assert!(!doc.set("Renderer", "fps_limit", "30"));
        let out = doc.render();
        assert!(out.contains("fps_limit\\default=false\r\nfps_limit=30\r\n\r\n[System]"));
        assert!(out.contains("backend=1"));
        assert_eq!(doc.get("renderer", "FPS_LIMIT").as_deref(), Some("30"));
    }

    #[test]
    fn adds_missing_keys_inside_the_section() {
        let mut doc = IniDoc::parse(SAMPLE);
        assert!(doc.set("Renderer", "frame_gen", "true"));
        let out = doc.render();
        assert!(out.contains("fps_limit=0\r\nframe_gen=true\r\n\r\n[System]"), "{out}");
    }

    #[test]
    fn adds_missing_sections() {
        let mut doc = IniDoc::parse("");
        assert!(doc.set("Renderer", "frame_gen", "true"));
        assert_eq!(doc.render(), "[Renderer]\r\nframe_gen=true\r\n");
    }

    #[test]
    fn keeps_lf_files_lf() {
        let mut doc = IniDoc::parse("[Renderer]\nfps_limit=0\n");
        doc.set("Renderer", "fps_limit", "30");
        assert_eq!(doc.render(), "[Renderer]\nfps_limit=30\n");
    }
}
