# Credits

Just Print Already is distributed under the GNU GPL v2 or later (see `LICENSE`), the license of the code it builds on.

| Path | Origin | Author(s) |
|---|---|---|
| `drivers/zjs/rastertozjs.c` | [Diego-Rebello/hp-m1132-arm64](https://github.com/Diego-Rebello/hp-m1132-arm64), itself from [faradayfury/printer-all](https://github.com/faradayfury/printer-all) | Diego Rebello, faradayfury; based on foo2zjs by Rick Richardson |
| `drivers/zjs/zjs.h` | foo2zjs, via hp-m1132-arm64 | Rick Richardson |
| `drivers/zjs/jbig*.{c,h}` | JBIG-KIT, via foo2zjs / hp-m1132-arm64 | Markus Kuhn |
| `drivers/zjs/template.ppd` | `ppd/HP-LaserJet_Professional_M1132_MFP-arm64.ppd` from hp-m1132-arm64, with model fields turned into placeholders | Diego Rebello |
| `drivers/testpage.pdf` | `test/ruler-a4.pdf` from hp-m1132-arm64 | Diego Rebello |

Driver sources are copied unmodified from hp-m1132-arm64 at commit `c353113`.
