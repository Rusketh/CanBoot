# Optional CanDo modules

Drop-in `.cdo` libraries that are **not** part of the bootable image by
default. CanBoot only bakes `/init.cdo` (plus the smoke-test probe
assets) onto the boot media. If you want one of these modules, copy it
onto your image and `include()` it from your init script — see each
module's doc for the exact steps.

| Module | What it is |
|--------|------------|
| [gui](gui/gui.md) | Retained-mode widget toolkit over `display` + `input` (mouse + keyboard) |

## Shipping a module onto a boot image

The image scripts under `scripts/` copy a fixed set of files. To add a
module, either drop it next to `init.cdo` in your own image build, or
`mcopy`/`cp` it into the staged root before the image is sealed, e.g.:

```bash
# FAT32 disk image
mcopy -i build/canboot-fat32.img modules/gui/gui.cdo ::/gui.cdo

# ISO staging dir (before xorriso/grub-mkrescue)
cp modules/gui/gui.cdo "$ISO_ROOT/gui.cdo"
```

Then from `/init.cdo`:

```cdo
VAR GUI = include("/gui.cdo");
```
