# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users
F4OLI, the operator of this fork, on his own Xiegu X6100. On the phone in the field (outdoors, sun, sometimes no
internet), on the PC at the desk next to the radio, and at night (in the shack or on an expedition, where a bright
page would dazzle).

## Product Purpose
The radio's web configurator (webserver/src/x6100_webserver): changes the radio's settings, lists and files from a
browser, works the radio remotely (Panel, Screen), and manages backups, updates and the network.

## Operating Context
Served by the radio itself (bottle, port 80/443) on the LAN, Tailscale, or the phone's hotspot. No internet can be
assumed: every asset (fonts included) is served from the radio. Running pages poll the radio; a lost link is a
normal state, not an exception.

## Capabilities and Constraints
- Fork only (OlivLoze/OpenX6X00): the look is ours, functions and copy stay upstream's so merges stay easy.
- The theme lives in its own stylesheet on top of base.css, so upstream changes to base.css keep merging.
- Rootfs space is limited: a few self-hosted woff2 files, no framework.

## Brand Commitments
The operator's house style ("charte Pi-station", the dashboard of his Pi apps): dark operator-console phosphor,
multi-colour never monochrome (amber, solar yellow, CW green, cobalt, magenta, red on near-black, cream text);
Big Shoulders Display for titles, IBM Plex Mono for small caps labels, JetBrains Mono for numbers; oscilloscope
graticule and film grain on the ground; cards with a 2px phosphor channel rail; dense padding.

## Product Principles
- Readable in the sun and gentle at night: contrast first, no large bright surfaces.
- One colour per area of the radio, the same in the menu, the page and its cards.
- Nothing hides behind the look: states (saved, failed, link lost) stay obvious.
