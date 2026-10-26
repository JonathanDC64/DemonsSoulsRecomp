# Privacy note

Version 1, 26 October 2026

This note covers this project's online server, the one the game uses when Settings > Network > Server is "Demon's
Souls Recomp". The project's maintainer runs it, on a DigitalOcean server in New York, United States. Offline play
sends nothing to it, and another server's owner decides what that server keeps.

## What the server keeps

When you play online, the game signs in to the server and sends it what the original game sent to its servers.

- **Your online name and your install's key.** The name is the one in Settings > Network. The key is made on your PC
  the first time you go online, and the server keeps its public half, which shows that later sign-ins are yours.
  Kept until you delete your data.
- **Your IP address**, with when it was first and last used. Kept 30 days after its last use. While a restriction on
  your name is in force, it is kept until the restriction ends.
- **Your characters' online records:** each save slot's co-op grades, how many co-op sessions it played, and how its
  messages were rated. Kept until you delete your data.
- **Your messages:** each character's newest 50. Older ones are deleted.
- **Your bloodstains:** 4 weeks, and only the newest 500 in each area.
- **World tendency:** each character's latest report, for 7 days.
- **Your ratings of other players' messages:** as long as the message is kept.
- **Who you played with** in co-op and invasions: 30 days, for your Players list.
- **Players you block:** until you unblock them.
- **Reports** you make, or that are made about you: 180 days.
- **Your sign-in sessions,** with the game's version: a session lasts a day.

Summon signs, wandering ghosts and co-op rooms stay in the server's memory only, for minutes.

The server's logs hold online names and what the game asked for, but no IP addresses. Backups of the server's data
are kept for 14 days, on the server and on the maintainer's PC.

## What other players see

- Your name, messages, bloodstains, ghosts and summon signs, as in the original game.
- In their Players list, your name and a short code made from your install's key, so a look-alike name shows a
  different code.
- Co-op and invasions connect your PC straight to the other player's, as on a PS3, so **the other player can see your
  IP address**.

## Restrictions

A player who breaks the server's rules can be restricted: banned, suspended for a time, or kept from reaching other
players. A restriction is kept with its reason and with the name and key it is about, and the maintainer's actions
are logged. These stay after you delete your data, so the restriction still holds.

## Deleting your data

Settings > Network > Delete my data deletes, at once:

- your characters' records, messages, bloodstains, world tendency reports and ratings;
- your IP addresses, who you played with, your blocks, and reports by you and about you;
- then your name and key, so anyone can take the name.

Only your own install can ask for this, with its key. Online play turns off afterwards, and turning it on again starts
a new record. Backups keep the old data until they are 14 days old.

If you no longer have the install, ask the maintainer on the project's GitHub page. Without the key, the maintainer
may not be able to tell that the name is yours.

## Other connections

- **The update check** asks GitHub for the newest release at each start, so GitHub sees your IP address. Turn it off
  in Settings > About.
- **Crash and problem reports** are saved on your PC. Nothing is sent unless you send it yourself.

## Changes

When this note changes in a way that matters, its version goes up, and the game asks you again before it goes online.
