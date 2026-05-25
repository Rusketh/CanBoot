# gui — retained-mode widget toolkit

A widget toolkit written entirely in CanDo on top of
[`display`](display.md) and [`input`](input.md). Unlike the other
entries here it is **not** a built-in namespace — it ships as `/gui.cdo`
on the boot image and is loaded with `include`:

```cdo
VAR GUI = include("/gui.cdo");

VAR f = GUI.Create("Frame");
f:SetSize(300, 200);
f:Center();
f:SetTitle("Hello");
f:MakePopup();

VAR b = GUI.Create("Button", f);
b:Dock(GUI.TOP);
b:SetText("Click me");
b.DoClick = FUNCTION(self) { print("clicked"); };

GUI.run();
```

The whole library is wrapped in an IIFE; the returned `GUI` table is the
only thing it adds to scope (nothing leaks into globals, and the widget
classes themselves are private — you reach them by name through
`GUI.Create`).

## Input model

CanBoot has no pointing device, so the on-screen cursor is driven by the
keyboard. The widget layer underneath is a real mouse model (hit-testing,
hover, `OnMousePressed`/`Released`, drag capture), so a future pointer
source drops in via `GUI.feedCursor` / `GUI.feedClick`.

| Action            | Key |
|-------------------|-----|
| Move cursor       | Arrow keys |
| Left click        | Enter |
| Cycle focus       | Tab (warps the cursor to the focused widget) |
| Cancel / close    | Esc (closes menus, blurs a field, closes the top frame) |
| Type / edit       | Printable keys + Backspace go to the focused text field |
| Drag a window     | Click its title bar to grab, move with arrows, Enter/Esc to drop |

In a focused `TextBox` (multi-line) the arrow keys move the caret; in a
single-line `TextEntry` only Left/Right are caret moves and Up/Down move
the cursor.

## Module functions

| Call | Effect |
|------|--------|
| `GUI.Create(class [, parent])` | Construct a registered widget; parents to `parent` or the root. |
| `GUI.getRoot()` | The screen-sized root panel that owns every top-level frame. |
| `GUI.run()` | Enter the event loop (blocks, pumping input + repainting). |
| `GUI.frame()` | Run one layout + think + paint cycle (for a custom loop). |
| `GUI.pump()` | Drain queued input without blocking. |
| `GUI.quit()` | Stop `GUI.run()`. |
| `GUI.Color(r, g, b [, a])` | A colour value (alpha is carried but the framebuffer is opaque). |
| `GUI.newline()` | A real newline string — the port does not interpret `"\n"` escapes. |
| `GUI.register(name, class)` | Register a custom widget class for `Create`. |
| `GUI.setBackend(display, input [, fb])` | Override the draw/input backends (used for testing). |
| `GUI.feedCursor(x, y)` / `GUI.feedClick(x, y)` | Drive the cursor / a click from an external source. |

Tunables: `GUI.cursorStep`, `GUI.dragStep`, `GUI.tickMs`,
`GUI.showCursor`. Theme colours live in `GUI.skin` (mutate before
building UI to retheme everything). Dock constants: `GUI.FILL`,
`GUI.LEFT`, `GUI.TOP`, `GUI.RIGHT`, `GUI.BOTTOM`, `GUI.NODOCK`. Key
codes: `GUI.keys.{UP,DOWN,LEFT,RIGHT,ENTER,ESC,TAB,BACKSP}`.

## Widgets

| Class | Notes |
|-------|-------|
| `Panel` | Base widget: position, size, parenting, docking, paint hooks. |
| `Frame` | Title bar, close button, drag, `SetTitle`, `MakePopup`, `Center`, `OnClose`. |
| `Label` | Text, `SetContentAlignment`, `SizeToContents`. |
| `Button` | `SetText`, hover/press states, `DoClick`. |
| `CheckBox` / `CheckBoxLabel` | `SetChecked`, `GetChecked`, `OnChange`. |
| `TextEntry` | Single-line editable text + caret, `GetValue`, `OnEnter`, `OnChange`, `SetNumeric`. |
| `TextBox` | Multi-line editable text area; `SetMultiline(bool)`, `GetLines`, `GetValue`, `OnChange`, `OnEnter` (single-line mode). |
| `NumberEntry` | Numeric text entry with `SetMinMax`, numeric `GetValue`. |
| `Progress` | `SetFraction`. |
| `Slider` | Draggable value slider, `SetMinMax`, `OnValueChanged`. |
| `NumSlider` | Label + slider + numeric readout. |
| `ComboBox` | `AddChoice`, `GetSelected`, `OnSelect` (opens a `Menu`). |
| `Menu` / `MenuOption` | Popup option list, `AddOption(text, fn)`. |
| `ScrollPanel` | Scrollable canvas with a vertical bar; `GetCanvas`, `AddItem`. |
| `ListView` | Columns + selectable rows, `AddColumn`, `AddLine`, `OnRowSelected`. |
| `PropertySheet` | Tabbed container, `AddSheet(label, panel)`, `SetActiveTab`. |
| `ColorMixer` | RGB sliders + swatch, `SetColor`, `GetColor`, `OnChange`. |

## Panel methods (common)

Geometry: `SetPos`/`GetPos`, `SetSize`/`GetSize`, `SetWide`/`SetTall`,
`GetWide`/`GetTall`, `Center`. Tree: `SetParent`, `Add`, `Remove`,
`GetChildren`, `LocalToScreen`/`ScreenToLocal`. Docking: `Dock(mode)`,
`DockMargin(l,t,r,b)`, `DockPadding(l,t,r,b)`, `SetZPos`. State:
`SetVisible`/`IsVisible`, `SetEnabled`/`IsEnabled`, `SetMouseInputEnabled`,
`SetKeyboardInputEnabled`, `RequestFocus`, `HasFocus`, `IsHovered`,
`MoveToFront`, `MakePopup`. Appearance: `SetText`, `SetTextColor`,
`SetContentAlignment`, `SetBGColor`, `SetPaintBackground`.

## TextBox

A multi-line editor. By default Enter inserts a newline and Up/Down move
the caret between rows; it scrolls vertically to keep the caret visible.
`SetMultiline(FALSE)` turns it into a single-line field (existing
newlines are flattened to spaces, Enter fires `OnEnter`, and Up/Down fall
through to move the cursor).

```cdo
VAR box = GUI.Create("TextBox", parent);
box:Dock(GUI.FILL);
box:SetValue("line one" + GUI.newline() + "line two");
box.OnChange = FUNCTION(self, text) { print("now: " + text); };

VAR single = GUI.Create("TextBox", parent);
single:SetMultiline(FALSE);
single.OnEnter = FUNCTION(self, text) { print("submit: " + text); };
```

`GetValue()` returns the full string (rows joined by `GUI.newline()`);
`GetLines()` returns the rows as an array.

## Hooks

Assign these as instance fields. **CanDo enforces exact arity**, so a
hook must take exactly the documented parameters (the leading `self` is
supplied by the `:` call):

| Hook | Signature |
|------|-----------|
| `Paint` / `PaintOver` | `(self, w, h)` |
| `PerformLayout` | `(self, w, h)` |
| `Think` | `(self)` |
| `DoClick` | `(self)` |
| `OnMousePressed` / `OnMouseReleased` | `(self, mouseCode, x, y)` |
| `OnCursorEntered` / `OnCursorExited` | `(self)` |
| `OnChange` | `(self, value)` |
| `OnValueChanged` | `(self, value)` |
| `OnSelect` | `(self, index, text, data)` |
| `OnRowSelected` | `(self, row, line)` |
| `OnEnter` | `(self, value)` |
| `OnFocusChanged` | `(self, gained)` |
| `OnClose` / `OnRemove` | `(self)` |

## Notes

- Text uses the framebuffer's fixed 8×8 font; `SetFont` is accepted for
  API parity but does not change glyph size.
- Colours are opaque `0xRRGGBB`; alpha is stored on `Color` values but
  solid fills ignore it (`GUI.blend` composites against a known
  background where needed).
- The pinned CanDo port does not interpret `"\n"` / `"\t"` escape
  sequences, hex/binary number literals, arrow functions, or unary `~`;
  the library sticks to what the port supports. Use `GUI.newline()` for
  literal newlines.
- The library is exercised headlessly against a mock framebuffer in the
  CanDo test harness; see the source header of `initramfs/gui.cdo`.

## See also

- [`display`](display.md) — the framebuffer painter it draws through
- [`input`](input.md) — the key codes it reads (incl. arrows + Esc)
- [`image`](image.md) — decode + draw images into panels
