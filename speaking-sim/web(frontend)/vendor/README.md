# vendor/

Third-party browser code, committed rather than fetched, so a page works with no
network beyond the examiner call and no package manager anywhere in the build.

| File | Version | Source | Licence |
| --- | --- | --- | --- |
| `htmx.esm.js` | 2.0.11 | `raw.githubusercontent.com/bigskysoftware/htmx/v2.0.11/dist/htmx.esm.js` | 0BSD |

The ES module build rather than the smaller minified one, which htmx ships only
as a classic script: `var htmx = function(){...}()`, so it publishes itself by
landing a global on the page. Imported instead, the pages that call `htmx.ajax`
name it at the top of the file like every other dependency, which is the whole
point of the module conversion - and 171 kB unminified costs nothing over
localhost.

Pinned to a tag rather than taken from `master`, the same way `CMakeLists.txt`
pins Crow, Asio, cpp-httplib and the SQLite amalgamation.

0BSD ("Zero-Clause BSD") asks for nothing in return - no attribution, no notice
kept beside the file - so unlike `third_party/` there is no licence text to
carry here. It is written down anyway, because knowing what a vendored file is
and where it came from is the whole point of this directory.

To update: fetch the new tag's `dist/htmx.min.js` over the top and change the
version above. `/vendor/<name>` serves it, and the name is held to an allowlist
in `src/static_files.cpp`, so a new file needs a line there too.
