# An Echo from Windows, step by step

This page is for you if you have never used a command line, git or adb, and want to turn an Echo into a Home
Assistant voice satellite from a **Windows** PC. Every command on this page is copied and pasted in full; nothing has
to be worked out. The examples are for the **Echo Dot 2nd gen**; another model works the same way with its own name
in the commands, and its steps differ a little (the setup names each one).

The setup on Windows starts from an Echo that is **unlocked already**: the unlock, and later boot-root, you do by
hand, as the model's XDA thread says. Everything else the setup does. Once the Echo is unlocked, that takes about an
hour: half an hour to install the tools, the rest mostly waiting.

> [!NOTE]
> The setup's steps after the unlock have run from Git Bash (issue #49). Nobody has reported the whole way from a new
> Echo yet. If you do it, please say how it went in
> [issue #67](https://github.com/Gamer92000/echo-dot-assist/issues/67), whether it worked or not: that is how this
> page gets better.

## 1. Check your Echo

Turn the Echo over: its **model number** is printed on the underside (the Echo Dot 2nd gen, 2016, is **RS03QR**).
Several Echos look alike, so go by that number, not by the looks, and find it in the
[Supported Echos](../README.md#supported-echos) table. Its row names the model's page; the setup needs the model's
name, the word in its row's link (`biscuit` for the Echo Dot 2).

You also need:

- A **Windows 10 or 11 PC**.
- A **USB way into the Echo**: the model's page says what (on the Echo Dot 2 a **micro-USB cable that carries data**;
  many cheap cables only charge, and one that has ever moved photos off a phone is a data cable).
- **Home Assistant** with a working voice assistant (Assist): try talking to it in the Home Assistant app first.
- **Wi-Fi** with a WPA2 password (the usual kind at home), on the same network as Home Assistant.

> [!IMPORTANT]
> **Do not set the Echo up with the Alexa app, and do not let it onto Wi-Fi before the unlock.** Amazon updates it
> as soon as it is online, and a newer version may not unlock any more.

## What the words mean

| Word | What it is |
|---|---|
| **PowerShell** | Windows' own command window. You use it only to install tools. |
| **Git Bash** | A second command window, installed with Git. The setup runs in it. |
| **adb** | Google's program that talks to Android devices over USB. The Echo runs a kind of Android. |
| **Unlock** | Lets the Echo run software that is not Amazon's. Done once, by hand, as the model's XDA thread says. |
| **XDA thread** | The forum thread of [R0rt1z2](https://github.com/R0rt1z2), who made the unlock: its files, every step and help when one goes wrong. The model's page links it. |
| **TWRP** | A small repair system the unlock puts on the Echo. On an Echo Dot its ring is white. |
| **boot-root** | R0rt1z2's zip from the same thread that gives the Echo a root adb. Installed by hand, as the thread says. |
| **The setup** | `scripts/setup.sh` from this project. It does everything in order and stops whenever it needs you. |

## Is it safe?

The setup **wipes the Echo**: Alexa is gone afterwards. The risky part is the unlock, and it is not this project's:
read the model's XDA thread in full first, and its warnings. The setup itself you can stop at any time with
**Ctrl+C**, and the next run carries on where it stopped.

## 2. Unlock the Echo, by hand

The setup does not unlock the Echo, on Windows or anywhere: R0rt1z2, who made the unlocks, asks that they are only ever
done by hand, as his XDA threads say. Open the model's page from the [Supported Echos](../README.md#supported-echos)
table, follow its link to the XDA thread, and go through the thread from its first post: which Echos it is for, which
files to download, what the PC needs and every step are there, kept up to date.

**Stop once TWRP is running on the Echo** (on an Echo Dot: a white ring). The setup flashes the firmware itself, and
asks you for boot-root when it is time. Stuck, or the Echo does something the thread does not say? Ask in the thread,
with what the Echo did, in your own words, and the tool's output or log as it is.

Then leave the Echo **connected to the PC by USB**, in TWRP: the setup starts from there. (Rooted it already, as the
thread says? Fine too: if it runs the firmware this project needs, the setup offers to skip the unlock, firmware and
root steps.)

## 3. Install the tools (once per PC)

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/windows-tools-dark.svg">
  <img alt="Three things to install, each with where and what to type. 1 Git for Windows, which brings Git Bash, the window the setup runs in: in PowerShell, winget install Git.Git. 2 Python, which the setup signs your updates with: the installer from python.org, with Add python.exe to PATH ticked. 3 Android platform-tools, adb and fastboot, which talk to the Echo over USB: winget install Google.PlatformTools. Then close every PowerShell and Git Bash window and open Git Bash anew: it only sees new tools when it starts." src="img/windows-tools-light.svg">
</picture>

**Opening PowerShell:** press the Windows key, type `PowerShell`, press Enter. To paste a command into PowerShell,
right-click in the window, then press Enter.

1. **Git for Windows.** In PowerShell:
   ```powershell
   winget install Git.Git
   ```
   If it asks you to agree to terms, type `Y` and Enter. The same goes for the other `winget` line below.
2. **Python.** Go to [python.org/downloads](https://www.python.org/downloads/), click the yellow "Download Python"
   button and run the file. On its first screen, **tick "Add python.exe to PATH"** at the bottom, then click
   "Install Now".
3. **adb.** In PowerShell:
   ```powershell
   winget install Google.PlatformTools
   ```

Then **close every PowerShell window**. From here on everything happens in Git Bash.

## 4. Get this project

Press the Windows key, type `Git Bash`, press Enter. A black window opens. **Pasting in Git Bash** is different:
**Shift+Insert**, or right-click and "Paste" (Ctrl+V does not work there). Paste these two lines and press Enter:

```sh
git clone https://github.com/Gamer92000/echo-dot-assist.git
cd echo-dot-assist
```

The first line copies the project into a folder `echo-dot-assist` in your user folder
(`C:\Users\<you>\echo-dot-assist`). The setup takes the Echo's software from the newest version GitHub has built and
published, so nothing has to be compiled on your PC.

Want to see first what the setup will do, without touching the Echo? `scripts/setup.sh biscuit --dry-run` walks
through every step and only shows the commands.

## 5. Run the setup

In the same Git Bash window, with the Echo unlocked and on USB (section 2):

```sh
scripts/setup.sh biscuit
```

(`biscuit` is Amazon's internal name for the Echo Dot 2; another model: its name from section 1.) It shows a
warning; type `yes` and Enter to start. No unlocked Echo on USB? It says so and stops: section 2 first.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="img/windows-run-dark.svg">
  <img alt="The setup's ten steps for an Echo Dot 2 on Windows. 1 Tools on this PC: by itself, it checks the tools and says which are missing. 2 Downloads: you save the firmware into Downloads, it picks it up from there. 3 USB access: skipped on Windows. 4 Micro-USB cable: on USB already since the unlock, checked. 5 Unlock, boot TWRP: done by hand before, as the XDA thread says, only checked, TWRP started. 6 Stock firmware into both slots: by itself, it wipes the Echo and writes the firmware into both slots. 7 Root: you install boot-root.zip by hand, as the XDA thread says, then press Enter and it checks. 8 Build: downloads GitHub's build and checks its signature. 9 Lock down, join Wi-Fi: you type your Wi-Fi's name and password. 10 Install: you press Enter at Settings to start with; the Echo restarts. Then add it in Home Assistant: it shows up as discovered." src="img/windows-run-light.svg">
</picture>

The setup goes through its steps and says each time what it does. Where it needs you:

- **Step 1, tools.** It says `all tools there`. If it says `missing:` instead, install what it names (section 3),
  close Git Bash, open it again and type:
  ```sh
  cd echo-dot-assist
  scripts/setup.sh biscuit
  ```
  That is how you **continue after any stop**: the setup remembers what is done. The same step also checks that
  GitHub has a published build of your copy (see "Something went wrong" if it says no).
- **Step 2, one download.** It shows a link and waits: the Echo's **firmware**, Amazon's own file. Save it to your
  normal **Downloads** folder; the setup notices it by itself and goes on. Do not rename it: it takes only this exact
  file. The one tool it needs besides (magiskpolicy, from Magisk's own release) it downloads itself later.
- **Steps 4 to 6** run by themselves: the Echo is on USB already, the setup starts TWRP if it is not running, and
  writes the firmware. Leave the cable in from here to the end.
- **Step 7, root.** It shows the XDA thread's link again and waits. Download `boot-root.zip` from the thread (XDA may
  ask you to sign in, with a free account) and install it on the Echo as the thread says, wait until the Echo has
  started its own software again, then press Enter in Git Bash. The setup checks that adb is root now.
- **Step 8** runs by itself.
- **Step 9, Wi-Fi.** It lists the networks the Echo hears. Type your Wi-Fi's name exactly as listed (capitals
  count), then its password. **The password does not show while you type**; that is on purpose. Press Enter.
- **Step 10, install.** It asks `Settings to start with`: press Enter. (This is for your second Echo: settings
  exported from the first.) The Echo restarts; the setup waits until the satellite runs.

At the end it says `"Echo Dot 2 …" is running at <address>` and what to do next. Now you can unplug the USB cable
and give the Echo its own power adapter.

## 6. Add it to Home Assistant

1. In Home Assistant: **Settings → Devices & services**. A discovered "ESPHome" device appears, named
   "Echo Dot 2" plus six letters and digits. Click **Add**. (Not there after a few minutes? "Add integration",
   ESPHome, enter the address the setup showed, port `26053`, no encryption key.)
2. On its device page pick an **Assist pipeline**, and give it a name of your choice.
3. Say **"Alexa"**.

Everything else is on the Echo's own **settings page**: `http://<its address>:28931/`, or "Visit" on its device
page in Home Assistant. A press of the Echo's action button lets your browser in. What is there:
[the guide](GUIDE.md#settings-page).

## 7. Keep one file safe

In `C:\Users\<you>\echo-dot-assist\secrets\` is **`update.key`**. Copy it somewhere safe (a USB stick, your cloud
storage). It is the key your Echo trusts for updates you send it yourself. Updates from this project's releases do
not need it: switch on "Online updates" on the settings page.

## Something went wrong

The setup writes everything it did to `C:\Users\<you>\echo-dot-assist\build\biscuit\setup.log`. Attach that file
when you ask for help with the setup. For the unlock and boot-root, ask in the model's XDA thread.

| What you see | What to do |
|---|---|
| `On Windows this needs an Echo that is unlocked already` | No Echo on adb over USB. Unlock it first, as the XDA thread says (section 2), leave it in TWRP with the USB cable in, and run the setup again. |
| `missing: python3` although you installed Python | Python's installer ran without "Add python.exe to PATH". Run it again: "Modify", "Next", tick "Add Python to environment variables", "Install". Open Git Bash anew. |
| `missing: adb` | Close Git Bash and open it anew (it only sees tools installed before it started). Still missing: run the `winget` line again. |
| `no release build of this commit on GitHub` | GitHub has not published the newest change yet: it does a few minutes after one. Wait a few minutes, then in Git Bash, in `echo-dot-assist`: `git checkout main`, `git pull`, and run the setup again. Still there: `git checkout "$(git tag -l 'v*' --sort=-creatordate \| head -1)"` takes the newest published version. |
| `bash: scripts/setup.sh: No such file or directory` | You are not in the project folder: `cd ~/echo-dot-assist` first. |
| The ring **restarts** again and again | That USB port cannot power the Echo. Use a port on the PC itself with nothing else plugged in next to it. |
| Step 7 says `No root adb` | boot-root is not in place yet, or the Echo has not started its own software again. Go on in the XDA thread, then press Enter to look again. |
| The setup stopped, or you closed the window | Open Git Bash, `cd echo-dot-assist`, `scripts/setup.sh biscuit`. It carries on. |
| A question takes no answer, and Ctrl+C does nothing either | Close the window, then go on as in the row above. Please say so in [issue #106](https://github.com/Gamer92000/echo-dot-assist/issues/106), with `setup.log`. |
| Home Assistant does not find the Echo | Add it by hand (section 6, step 1). The Echo and Home Assistant have to be on the same network. |

**Alexa back?** Going back to Amazon's software is a matter for the model's XDA thread.

Everything this page leaves out, and the same steps for Linux: the model's page
([devices/biscuit/README.md](../devices/biscuit/README.md) for the Echo Dot 2) and the [README](../README.md).
