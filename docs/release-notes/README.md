# Release notes

One file per tag: `vX.Y.Z.md`. The release workflow (`.github/workflows/build-release.yml`) copies it into the GitHub
release body between `<!-- mania-notes -->` markers, and the console's updater shows it (UPDATES tab, WHAT'S NEW)
before the player installs.

- 3 to 6 short lines, each starting with `- `. Plain text for the player, in English, under ~2.5 KB. The updater
  draws it in a 42-column text view: no tables, links or images.
- A **stable** release lists everything since the previous *stable* tag, betas included. A **beta** lists only what
  is new since the previous tag.
- Written when the tag is made and merged before the tag exists.
