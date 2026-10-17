# Key-hint overlay images

Put the hint pages here as **PNG** files, then list them in
`../settings/default.vrsettings` under `shiyan_nakedmonitor_help.images`
(comma separated, one file per page):

```json
"shiyan_nakedmonitor_help": {
    "enabled": true,
    "images": "help_1.png,help_2.png,help_3.png",
    "width_ratio": 0.9
}
```

Behaviour:

* **V** shows / hides the overlay (direct mode only).
* While it is visible, **A / D** or **Left / Right** page through the images
  (wrapping); those keys are not forwarded to the input simulator.
* Each page is drawn centered in **both eye halves**, aspect-fit, at
  `width_ratio` (default 90%) of a half screen.

PNG with alpha is supported (it is alpha-blended over the SBS image).
Missing files are logged in `vrserver.txt` and simply skipped.
