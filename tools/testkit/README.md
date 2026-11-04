# Tester kit

A short, automatic test run of the game on your PC. It tells the developers how each renderer looks and runs on your
hardware.

## Before you start

- Finish the game's setup, and play until the game has saved at least once.
- Close the game.
- Allow about 10 minutes. Leave the PC alone while it runs: the game window opens and closes four times.

## Run it

Get this folder (`tools/testkit`) from the repository, or from the release's source zip, and give it the game's folder
(the one you start the game from):

```
bash run_tests.sh "<the game's folder>"
```

For each renderer, the game starts, loads a copy of your save, turns the camera around where you stand, saves five
screenshots and records its frame times, then closes by itself. Each renderer runs twice: first with an empty shader
cache (the first start's stutter), then with the cache the first run made.

## What it does not do

- It never writes your save: it plays from a copy, and deletes the copy afterwards.
- It sends nothing anywhere.
- Your settings stay as they are, except that the runs are muted and at the original 30 fps.

## Afterwards

The script prints a line for each run and the name of a file, `des_test_results_<date>.tar.gz`, beside the script.
Send that file to the developers. Before packing it, the script takes your user name, computer name and home folder
out of the logs. If it finds any left, it packs nothing and lists the files.

The file holds:
- the logs;
- the screenshots;
- the frame times (`summary.json`: fps, 1% lows, hitches);
- a few lines about your system: OS, CPU, RAM, GPU and driver.
