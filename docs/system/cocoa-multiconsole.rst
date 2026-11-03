.. SPDX-License-Identifier: GPL-2.0-or-later

Cocoa multiple-console layouts
=============================

The experimental Cocoa frontend accepts ``-display cocoa,split=on`` for two
independent panes and ``-display cocoa,layout=2+3`` for five panes. The first
two consoles occupy the left column and the other three the right column.

Create one ``-chardev vc,id=...`` per console and connect each to a separate
UART. Select a pane by clicking it; keyboard input, the View menu and scroll
position apply to that pane. The normal single-console layout remains the
default. These options require a Cocoa-enabled macOS build.

For a multiarch platform, the UART chardev is selected with the domain's
``chardev=`` property. Terminal dimensions can be specified through the VC
backend's ``cols=`` and ``rows=`` properties. This UI change does not change
the guest CPU, interrupt or address-space model.

The separate ``ruikoi-labs/projects/qemu-d9m`` project provides dual-Linux and
two-Linux/three-FreeRTOS examples, firmware build instructions, and a manual
input/focus/scrolling acceptance checklist.
