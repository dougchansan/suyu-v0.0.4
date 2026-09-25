# Next release notes (draft)

## Faster Vulkan pipeline setup

On supported Vulkan drivers, suyu now reuses compiled graphics pipeline parts and links a
new pipeline quickly when it is first needed. This reduces long waits caused by graphics
pipeline creation. Suyu builds a fully optimized version in the background after the
first use.

The change is enabled only when the driver reports fast graphics pipeline library
support. Other devices keep the previous path automatically; this includes the tested
macOS MoltenVK build. Android also keeps the previous path by default. If a supported
driver has a rendering problem, turn off **Use graphics pipeline libraries** in the
Vulkan settings.

This improves pipeline waits; loading transitions can still pause for other reasons.
