# PME

**P**rotect **M**y **E**yes. A simple tool that tries to alarm you
when you stick to the screen for too long time.
Idle time are taken into account using
[ext-idle-notify-v1](https://wayland.app/protocols/ext-idle-notify-v1)
protocol. And because of that, a wayland compositor supporting
ext-idle-notify-v1 is needed.

## Installation

### Requirements
 - meson \*
 - wayland
 - wayland-protocols \*

*\* Build dependency*

### Compilation

```sh
meson setup build/
ninja -C build/
```
Excutable located at `build/pme`.

## Credits&Notes

Code are mainly taken from
[swayidle](https://github.com/swaywm/swayidle).
This tool is crafted for learning wayland protocols and programming purpose.
In other words, it's just a toy, but at least it works. :)

