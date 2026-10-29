//! The native menu bar: craft-menu's menus (the app's, laid out as on Linux: Quit last in File,
//! Preferences last in Edit, About last in Help) in the window's `BMenuBar`, as the macOS build
//! has them in AppKit's. The in-window menus hide while it is there.
//!
//! [`describe`] turns a laid-out [`MenuBar`] into the lines `host.cpp` builds its menus from;
//! [`HaikuMenu`] is craft-menu's [`craft_menu::Backend`] for it. A chosen item comes back
//! through the host's event queue as its command's key and is reported as craft-menu reports a
//! click, so it runs through the app's menu commands like an in-window click.
//!
//! Shortcuts are shown the Haiku way, with the Command key (Alt, or Ctrl when the keymap swaps
//! them) for craft-menu's `cmd`/`ctrl`, Option for `alt`. Their keys still reach egui as keys
//! (`host.cpp` hands every Command key to the interface before `BWindow` looks for a menu
//! shortcut), so a shortcut runs through the app's key handling as without a native menu,
//! the way craft-menu feeds AppKit's key equivalents back to egui. Shortcuts without Command (a
//! tool key, a bare F-key or Delete) aren't shown: as `BWindow` shortcuts they would take those
//! keys from text fields, as they would from AppKit's.

use std::collections::HashMap;

use craft_menu::{Builtin, Chord, CommandRef, MenuBar, Node};

use crate::keys::{B_COMMAND_KEY, B_OPTION_KEY, B_SHIFT_KEY};

/// What an item does when chosen: its command, and what craft-menu needs to report it.
#[derive(Clone, Debug, PartialEq)]
pub struct Chosen {
    pub command: CommandRef,
    pub shortcut: Option<Chord>,
    pub layout_shortcut: bool,
}

/// The unit separator between a line's fields.
const SEP: char = '\u{1f}';

/// Text for one field: no line breaks or separators.
fn field(text: &str) -> String {
    text.chars().map(|c| if c == '\n' || c == '\r' || c == SEP { ' ' } else { c }).collect()
}

/// The character and modifiers `BMenuItem` shows for a shortcut, if it has Command and a key a
/// menu can draw: letters, digits, punctuation, arrows, Enter, Tab, Space, Backspace, Delete and
/// Home (Haiku's menus have no glyph for F-keys, Escape, End, Page Up and Down or Insert).
pub fn shortcut(chord: &Chord) -> Option<(u8, u32)> {
    let m = chord.mods;
    if !(m.cmd || m.ctrl) {
        return None;
    }
    let name = chord.key.name();
    let key = match name {
        "Left" => 0x1c,
        "Right" => 0x1d,
        "Up" => 0x1e,
        "Down" => 0x1f,
        "Enter" => 0x0a,
        "Tab" => 0x09,
        "Space" => 0x20,
        "Backspace" => 0x08,
        "Delete" => 0x7f,
        "Home" => 0x01,
        _ => match name.as_bytes() {
            [c] if c.is_ascii_graphic() => c.to_ascii_uppercase(),
            _ => return None,
        },
    };
    let mut bits = B_COMMAND_KEY;
    if m.shift {
        bits |= B_SHIFT_KEY;
    }
    if m.alt {
        bits |= B_OPTION_KEY;
    }
    Some((key, bits))
}

/// The lines `host.cpp` builds the menu bar from (see `ParseMenuSpec` there), and what each
/// item's command key stands for.
pub fn describe(bar: &MenuBar) -> (String, HashMap<String, Chosen>) {
    fn nodes(children: &[Node], out: &mut String, items: &mut HashMap<String, Chosen>) {
        for node in children {
            match node {
                Node::Item(it) => {
                    let key = it.command.key();
                    let flags = i32::from(it.enabled) | if it.kind.checked() == Some(true) { 2 } else { 0 };
                    // Quit shows Command+Q, as in every Haiku application: `BWindow` quits on it.
                    let quit = (Builtin::of(&it.command) == Some(Builtin::Quit)).then_some((b'Q', B_COMMAND_KEY));
                    let (c, m) = it.shortcut.as_ref().and_then(shortcut).or(quit).unwrap_or((0, 0));
                    out.push_str(&format!("I{SEP}{}{SEP}{}{SEP}{flags}{SEP}{c}{SEP}{m}\n", field(&key), field(&it.label)));
                    items.entry(key).or_insert_with(|| Chosen {
                        command: it.command.clone(),
                        shortcut: it.shortcut,
                        layout_shortcut: it.layout_shortcut,
                    });
                }
                Node::Separator => out.push_str("-\n"),
                Node::Submenu { label, children, .. } => {
                    out.push_str(&format!("S{SEP}{}\n", field(label)));
                    nodes(children, out, items);
                    out.push_str("E\n");
                }
                // AppKit's own items (Services, Zoom…): only the Mac layout has them.
                Node::Standard { .. } => {}
            }
        }
    }
    let mut out = String::new();
    let mut items = HashMap::new();
    for menu in &bar.menus {
        out.push_str(&format!("M{SEP}{}\n", field(&menu.title)));
        nodes(&menu.children, &mut out, &mut items);
        out.push_str("E\n");
    }
    (out, items)
}

/// The menus' shape: when it changes, `host.cpp` builds them again rather than updating labels
/// and states in place.
pub fn shape(bar: &MenuBar) -> (u64, u64) {
    (craft_menu::structure_key(bar), craft_menu::window_key(bar))
}

#[cfg(target_os = "haiku")]
pub use backend::{HaikuMenu, Picks};

#[cfg(target_os = "haiku")]
mod backend {
    use std::collections::HashMap;
    use std::sync::{Arc, Mutex, PoisonError};

    use craft_menu::{Backend, Dispatch, Event, MenuBar};

    use super::{Chosen, describe, shape};
    use crate::host;

    /// The items chosen in the menu bar since the last frame, as their command keys: the shell
    /// adds them as the host reports them, the backend takes them each frame.
    #[derive(Clone, Default)]
    pub struct Picks(Arc<Mutex<Vec<String>>>);

    impl Picks {
        pub fn push(&self, key: String) {
            self.0.lock().unwrap_or_else(PoisonError::into_inner).push(key);
        }

        fn take(&self) -> Vec<String> {
            std::mem::take(&mut *self.0.lock().unwrap_or_else(PoisonError::into_inner))
        }
    }

    /// craft-menu's backend for the window's `BMenuBar`.
    pub struct HaikuMenu {
        picks: Picks,
        dispatch: Option<Dispatch>,
        items: HashMap<String, Chosen>,
        shape: Option<(u64, u64)>,
    }

    impl HaikuMenu {
        pub fn new(picks: Picks) -> Self {
            Self { picks, dispatch: None, items: HashMap::new(), shape: None }
        }
    }

    impl Backend for HaikuMenu {
        fn sync(&mut self, bar: &MenuBar) {
            let shape = shape(bar);
            let (spec, items) = describe(bar);
            host::menu_set(&spec, self.shape != Some(shape));
            self.shape = Some(shape);
            // An item chosen before this sync and gone from it is dropped when it arrives.
            self.items = items;
        }

        fn drain(&mut self) -> Vec<Event> {
            let Some(dispatch) = &self.dispatch else { return Vec::new() };
            self.picks
                .take()
                .into_iter()
                .filter_map(|key| self.items.get(&key))
                // A mouse click, or Enter in an open menu: the item's key equivalents reach egui
                // as keys and never come this way.
                .map(|c| dispatch.chosen(&c.command, c.shortcut.as_ref(), c.layout_shortcut, None))
                .collect()
        }

        fn tracking(&self) -> bool {
            host::menu_tracking()
        }

        fn attach(&mut self, dispatch: Dispatch) {
            self.dispatch = Some(dispatch);
        }
    }
}

#[cfg(test)]
mod tests {
    use craft_menu::{Item, Menu, MenuRole};

    use super::*;

    fn chord(text: &str) -> Chord {
        Chord::parse(text).unwrap_or_else(|e| panic!("{text}: {e:?}"))
    }

    #[test]
    fn shortcuts_with_command_show_with_haikus_modifiers() {
        assert_eq!(shortcut(&chord("Ctrl+S")), Some((b'S', B_COMMAND_KEY)));
        assert_eq!(shortcut(&chord("Ctrl+Shift+Z")), Some((b'Z', B_COMMAND_KEY | B_SHIFT_KEY)));
        assert_eq!(shortcut(&chord("Ctrl+Alt+P")), Some((b'P', B_COMMAND_KEY | B_OPTION_KEY)));
        assert_eq!(shortcut(&chord("Ctrl+=")), Some((b'=', B_COMMAND_KEY)));
        assert_eq!(shortcut(&chord("Ctrl+0")), Some((b'0', B_COMMAND_KEY)));
        assert_eq!(shortcut(&chord("Ctrl+Left")), Some((0x1c, B_COMMAND_KEY)));
    }

    #[test]
    fn tool_keys_and_keys_menus_cant_draw_are_not_shown() {
        // Without Command they'd become window shortcuts taking the key from text fields.
        assert_eq!(shortcut(&chord("B")), None);
        assert_eq!(shortcut(&chord("Shift+B")), None);
        assert_eq!(shortcut(&chord("F5")), None);
        assert_eq!(shortcut(&chord("Delete")), None);
        // Haiku's menus have no glyph for these.
        assert_eq!(shortcut(&chord("Ctrl+F4")), None);
        assert_eq!(shortcut(&chord("Ctrl+PageDown")), None);
    }

    /// A small bar: `open` labels File ▸ Open, `full_screen` is View ▸ Full screen's state, and
    /// `save` adds File ▸ Save.
    fn bar(open: &str, full_screen: bool, save: bool) -> MenuBar {
        let mut file = vec![
            Item::new("file.open", open).shortcut(chord("Ctrl+O")).into(),
            Node::Separator,
            Node::submenu(
                "Open Recent",
                vec![Item::new(CommandRef::with_params("file.open_recent", serde_json::json!({ "index": 0 })), "a\tb\u{1f}.pdf").into()],
            ),
        ];
        if save {
            file.push(Item::new("file.save", "Save").into());
        }
        file.push(Item::new(Builtin::Quit.command(), "Quit").into());
        MenuBar {
            menus: vec![
                Menu::new("File", file).role(MenuRole::File),
                Menu::new("View", vec![Item::new("view.full_screen", "Full screen").enabled(false).check(full_screen).into()]).role(MenuRole::View),
            ],
            documents: Vec::new(),
        }
    }

    #[test]
    fn the_description_lists_menus_items_and_states_in_order() {
        let (spec, items) = describe(&bar("Open…", true, false));
        let lines: Vec<Vec<&str>> = spec.lines().map(|l| l.split(SEP).collect()).collect();
        let expected: Vec<Vec<&str>> = vec![
            vec!["M", "File"],
            vec!["I", "file.open", "Open…", "1", "79", "2"],
            vec!["-"],
            vec!["S", "Open Recent"],
            vec!["I", "file.open_recent {\"index\":0}", "a\tb .pdf", "1", "0", "0"],
            vec!["E"],
            vec!["I", "craft-quit.quit", "Quit", "1", "81", "2"],
            vec!["E"],
            vec!["M", "View"],
            vec!["I", "view.full_screen", "Full screen", "2", "0", "0"],
            vec!["E"],
        ];
        assert_eq!(lines, expected);
        let recent = items.get("file.open_recent {\"index\":0}").map(|c| &c.command);
        assert_eq!(recent, Some(&CommandRef::with_params("file.open_recent", serde_json::json!({ "index": 0 }))));
        assert_eq!(items.get("file.open").and_then(|c| c.shortcut), Some(chord("Ctrl+O")));
    }

    #[test]
    fn a_new_label_or_state_keeps_the_shape_and_a_new_item_changes_it() {
        let before = bar("Open…", true, false);
        assert_eq!(shape(&before), shape(&bar("Öffnen…", false, false)));
        let grown = bar("Open…", true, true);
        assert_ne!(shape(&before), shape(&grown));
    }
}
