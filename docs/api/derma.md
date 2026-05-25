# derma — Garry's Mod Derma-style GUI

A retained-mode widget toolkit modelled on Garry's Mod's Derma/VGUI,
written entirely in CanDo on top of [`display`](display.md) and
[`input`](input.md). Unlike the other entries here it is **not** a
built-in namespace — it ships as `/derma.cdo` on the boot image and is
loaded with `include`:

```cdo
VAR derma = include("/derma.cdo");

VAR f = derma.Create("DFrame");
f:SetSize(300, 200);
f:Center();
f:SetTitle("Hello");
f:MakePopup();

VAR b = derma.Create("DButton", f);
b:Dock(derma.TOP);
b:SetText("Click me");
b.DoClick = FUNCTION(self) { print("clicked"); };

derma.run();
```

The whole library is wrapped in an IIFE; the returned `derma` table is
the only thing it adds to scope (nothing leaks into globals).

## Input model

CanBoot has no pointing device, so the on-screen cursor is driven by the
keyboard. The widget layer underneath is a real mouse model (hit-testing,
hover, `OnMousePressed`/`Released`, drag capture), so a future pointer
source drops in via `derma.feedCursor` / `derma.feedClick`.

| Action            | Key |
|-------------------|-----|
| Move cursor       | Arrow keys |
| Left click        | Enter |
| Cycle focus       | Tab (warps the cursor to the focused widget) |
| Cancel / close    | Esc (closes menus, blurs a field, closes the top frame) |
| Type / edit       | Printable keys + Backspace go to the focused text field |
| Drag a window     | Click its title bar to grab, move with arrows, Enter/Esc to drop |

## Module functions

| Call | Effect |
|------|--------|
| `derma.Create(class [, parent])` | Construct a registered widget; parents to `parent` or the root. |
| `derma.getRoot()` | The screen-sized root panel that owns every top-level frame. |
| `derma.run()` | Enter the event loop (blocks, pumping input + repainting). |
| `derma.frame()` | Run one layout + think + paint cycle (for a custom loop). |
| `derma.pump()` | Drain queued input without blocking. |
| `derma.quit()` | Stop `derma.run()`. |
| `derma.Color(r, g, b [, a])` | A colour value (alpha is carried but the framebuffer is opaque). |
| `derma.register(name, class)` | Register a custom widget class for `Create`. |
| `derma.setBackend(display, input [, fb])` | Override the draw/input backends (used for testing). |
| `derma.feedCursor(x, y)` / `derma.feedClick(x, y)` | Drive the cursor / a click from an external source. |

Tunables: `derma.cursorStep`, `derma.dragStep`, `derma.tickMs`,
`derma.showCursor`. Theme colours live in `derma.skin` (mutate before
building UI to retheme everything). Dock constants: `derma.FILL`,
`derma.LEFT`, `derma.TOP`, `derma.RIGHT`, `derma.BOTTOM`, `derma.NODOCK`.

## Widgets

| Class | Notes |
|-------|-------|
| `Panel` / `DPanel` | Base widget: position, size, parenting, docking, paint hooks. |
| `DFrame` | Title bar, close button, drag, `SetTitle`, `MakePopup`, `Center`, `OnClose`. |
| `DLabel` | Text, `SetContentAlignment`, `SizeToContents`. |
| `DButton` | `SetText`, hover/press states, `DoClick`. |
| `DCheckBox` / `DCheckBoxLabel` | `SetChecked`, `GetChecked`, `OnChange`. |
| `DTextEntry` | Editable text + caret, `GetValue`, `OnEnter`, `OnChange`, `SetNumeric`. |
| `DNumberWang` | Numeric text entry with `SetMinMax`, numeric `GetValue`. |
| `DProgress` | `SetFraction`. |
| `DSlider` | Draggable value slider, `SetMinMax`, `OnValueChanged`. |
| `DNumSlider` | Label + slider + numeric readout. |
| `DComboBox` | `AddChoice`, `GetSelected`, `OnSelect` (opens a `DMenu`). |
| `DMenu` / `DMenuOption` | Popup option list, `AddOption(text, fn)`. |
| `DScrollPanel` | Scrollable canvas with a vertical bar; `GetCanvas`, `AddItem`. |
| `DListView` | Columns + selectable rows, `AddColumn`, `AddLine`, `OnRowSelected`. |
| `DPropertySheet` | Tabbed container, `AddSheet(label, panel)`, `SetActiveTab`. |
| `DColorMixer` | RGB sliders + swatch, `SetColor`, `GetColor`, `OnChange`. |

## Panel methods (common)

Geometry: `SetPos`/`GetPos`, `SetSize`/`GetSize`, `SetWide`/`SetTall`,
`GetWide`/`GetTall`, `Center`. Tree: `SetParent`, `Add`, `Remove`,
`GetChildren`, `LocalToScreen`/`ScreenToLocal`. Docking: `Dock(mode)`,
`DockMargin(l,t,r,b)`, `DockPadding(l,t,r,b)`, `SetZPos`. State:
`SetVisible`/`IsVisible`, `SetEnabled`/`IsEnabled`, `SetMouseInputEnabled`,
`SetKeyboardInputEnabled`, `RequestFocus`, `HasFocus`, `IsHovered`,
`MoveToFront`, `MakePopup`. Appearance: `SetText`, `SetTextColor`,
`SetContentAlignment`, `SetBGColor`, `SetPaintBackground`.

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
  solid fills ignore it (`derma.blend` composites against a known
  background where needed).
- The library is exercised headlessly against a mock framebuffer in the
  CanDo test harness; see the source header of `initramfs/derma.cdo`.

## See also

- [`display`](display.md) — the framebuffer painter it draws through
- [`input`](input.md) — the key codes it reads (incl. arrows + Esc)
- [`image`](image.md) — decode + draw images into panels
