# gui — retained-mode widget toolkit

A self-contained CanDo GUI library that paints through the
[`display`](../../docs/api/display.md) framebuffer and is driven by
[`input`](../../docs/api/input.md) (keyboard **and** mouse). It is an
optional module: it is not baked into the boot image — copy `gui.cdo`
onto your media and `include()` it yourself.

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
only name it introduces. Widget classes are private and created by name
through `GUI.Create`, so there are no global collisions.

## Files

- `gui.cdo` — the library (include this).
- `gui_demo.cdo` — a showcase; co-locate it with `gui.cdo` and run it as
  `/init.cdo`, or `include("./gui_demo.cdo")` from your own init.

## Getting it onto a boot image

The build scripts don't ship it. Stage it next to `init.cdo` yourself:

```bash
mcopy -i build/canboot-fat32.img modules/gui/gui.cdo ::/gui.cdo   # FAT32
cp modules/gui/gui.cdo "$ISO_ROOT/gui.cdo"                        # ISO root
```

## Input

A mouse is used when a pointing device is present (PS/2 or virtio); the
cursor falls back to the keyboard otherwise.

| Action          | Mouse            | Keyboard |
|-----------------|------------------|----------|
| Move cursor     | move             | arrow keys |
| Click / press   | left button      | Enter |
| Drag            | hold + move      | click to grab, arrows, Enter/Esc to drop |
| Scroll          | wheel            | drag the scrollbar |
| Focus next      | —                | Tab |
| Cancel / close  | click off / Esc  | Esc |
| Type / edit     | —                | printable keys + Backspace into the focused field |

## Widgets

`Panel`, `Frame`, `Label`, `Button`, `CheckBox`, `CheckBoxLabel`,
`TextEntry`, `TextBox` (multi-line, `SetMultiline`), `NumberEntry`,
`Progress`, `Slider`, `NumSlider`, `ComboBox`, `Menu`, `ScrollPanel`,
`ListView`, `PropertySheet`, `ColorMixer`.

Full API — widget table, hook signatures, theming — is in
[`docs/api/gui.md`](../../docs/api/gui.md).

## Notes

- The library is verified headlessly against a mock framebuffer + mock
  input in the CanDo desktop interpreter (80 assertions incl. mouse drag
  and click edge-detection).
- The pinned CanDo port does not interpret `"\n"` escapes, hex/binary
  literals, arrow functions, or unary `~`; the library avoids all of
  them and exposes `GUI.newline()` for literal newlines.
