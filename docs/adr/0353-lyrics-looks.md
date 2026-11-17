# ADR-0353: Looks for the Lyrics panel

## Status

Accepted, 2026-11-17. Extends the Lyrics panel of ADR-0276.

## Context

- **The Lyrics panel had one look**: lines centred in the list's font, the
  one sung bold and the rest dimmed. It suits a narrow panel beside the
  lists, not a wide one to sing along to, nor reading the words through.
- **Its look cannot come from a plugin.** Lyrics is Melody's own panel, and
  plugins style nothing (ADR-0304); what a lyrics provider answers is text.
- **People want their own colours and type**, the more so for a panel
  that is mostly words.

## Decision

1. **Four looks, Melody's own**, chosen from a menu at the right of the
   panel's heading (or a right click on it), drawn at once and kept:
   - **Page** -- as it always was: centred, the line sung bold, the rest
     dimmed alike. The look until another is chosen.
   - **Stage** -- large type, the line sung a little larger still, the
     others fading the further they are from it.
   - **Sleeve** -- the playing album's cover, blurred and veiled, behind the
     whole panel, the words light on it; with no cover, the veil alone.
   - **Reading** -- left, close together, nothing dimmed: the line sung
     only in the accent colour.
2. **Each look keeps its own font, size and colours.** Font and colours…,
   in the same menu, sets the look shown: a font, a size (50 to 300 %),
   the colour of the lines, of the line sung, and of the background -- over
   the cover, a veil whose opacity is held below the wall it would
   otherwise be. Each is the look's own until set, and can be given back.
   Every change is drawn in the panel as it is made, a colour while it is
   still being picked; OK keeps them, Cancel goes back.
3. **Kept in Melody's settings**, `lyrics/look` and
   `lyrics/looks/<look>/{font,size,lines,sung,background}`; nothing is kept
   that is the look's own, so a look improved later reaches those who never
   changed it.
4. **Colour schemes still apply** where nothing is set: a look's colours are
   the scheme's, but for Sleeve, which is drawn on the cover.

## Not in this decision

- Looks from plugins, or a look of one's own beyond these four's settings.
- Word timings (karaoke filling within a line): the engine drops them
  (ADR-0276).
- A full-screen lyrics view.

## Verification

- `bench-main-window`: Page as before; a look chosen from the menu is
  kept; Stage larger and receding with distance; Reading left with the
  accent; a look's colours and size kept for it alone; the dialog previews,
  Cancel goes back, OK keeps; Sleeve draws the cover behind light words
  and no cover once there is none.
