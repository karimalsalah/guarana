# User Handbook - Installing Mods

## General Notes

**Note:** When installing a mod, always read the mod's installation instructions.

This is especially important if a mod uses a framework such as **ARCropolis**, **Skyline**, or **Atmosphere plugins**. In those cases, follow the framework's instructions instead of using Eden's normal mod folder.

For example, **Super Smash Bros. Ultimate** uses such a framework. See the related section below for details.

---

# Installing Mods for Most Games

The easy way:

1. Right click a game in the game list and click **Configure Game**.
2. In the **Add-Ons** tab, click **Import Mod from ZIP** or **Import Mod from Folder** and pick the mod.

Guarana copies the mod into the game's mod folder and leaves the file or folder you picked as it is. If a mod of the same name is already installed, it asks before it replaces that mod. Each imported mod gets a small `mod.json` with its name, what it was imported from and when. The game never sees that file.

By hand:

1. Right click a game in the game list and click **"Open Mod Data Location"**, or click **Open Mods Folder** in the Add-Ons tab.
2. Extract the mod into that folder.

Each mod should be placed inside **its own subfolder**.

## When two mods change the same file

Mods are applied in the order of their names. When two ticked mods replace the same file of the game, the game gets the file of the mod that comes first. The Add-Ons tab lists such files under the list of mods, with the mod whose file is taken. To get the other mod's file, untick the first mod, or rename the mods' folders so that the one you want comes first.

---

# Enabling or Disabling Mods

1. Right click the game in the game list.
2. Click **Configure Game**.
3. In the **Add-Ons** tab, enable or disable mods, updates, and DLC by ticking or unticking their boxes.

---

# Built-in Patches

Some games come with patches that are part of Guarana. They need no download and are in the same **Add-Ons** list, with **Built-in** in the Version column:

- **Built-in, recommended**: leave it on. The game misbehaves without it.
- **Built-in, experimental**: new or little tested, and off until you tick it.
- **Built-in**: no advice either way. Read what it does and choose.

Click a built-in patch to read under the list what it does. Once the game has been started, the same box says whether the patch was applied. A warning sign next to a ticked patch means it was not, and the box says why:

- **It is made for another version of the game.** The patch does nothing for the version you have.
- **A mod changes the same part of the game.** Turn that mod off to use the patch.
- **The game's code is not what it expects.** The patch does nothing for this copy of the game.

Guarana knows this from the last time the game started, and forgets it when it is closed.

---

# Important Note About SD Card Paths

Some mods are designed for real Nintendo Switch consoles and refer to the **SD card root**.

The emulated SD card is located at:

```
%AppData%\guarana\sdmc
```

Example:

```
Switch instruction:  sd:/ultimate/mods
Eden equivalent:     sdmc/ultimate/mods
```

---

# Framework-Based Mods (Super Smash Bros. Ultimate)

Some games require external mod frameworks instead of the built-in mod loader.

The most common example is **Super Smash Bros. Ultimate**.

These mods are installed directly to the **emulated SD card**, not the normal Eden mod folder.

---

# Installing the ARCropolis Modding Framework

**Note:** Some mod packs bundle ARCropolis with their installer (for example, Smash Ult-S).

---

## 1. Download ARCropolis

Download the latest release:

https://github.com/Raytwo/ARCropolis/releases/

---

## 2. Install ARCropolis

Extract the **`atmosphere`** folder into:

```
%AppData%\guarana\sdmc
```

This is the **emulated SD card directory**.

Verify installation by checking that the following file exists:

```
sdmc\atmosphere\contents\01006A800016E000\romfs\skyline\plugins\libarcropolis.nro
```

---

## 3. Download Skyline

Download the latest Skyline release:

https://github.com/skyline-dev/skyline/releases

Skyline used to be bundled with ARCropolis but is now distributed separately to avoid compatibility issues caused by outdated bundled versions.

---

## 4. Install Skyline

Extract the **`exefs`** folder into:

```
sdmc\atmosphere\contents\01006A800016E000
```

The `exefs` folder should be **next to the `romfs` folder**.

Verify installation by checking that the following file exists:

```
%AppData%\guarana\sdmc\atmosphere\contents\01006A800016E000\exefs\subsdk9
```

---

## 5. Launch the Game Once

Start the game and make sure you see the **ARCropolis version text on the title screen**.

This will also create the folders required for installing mods.

---

## 6. Install Smash Ultimate Mods

Install mods inside:

```
sdmc\ultimate\mods
```

Each mod must be placed inside **its own subfolder**.

Example:

```
sdmc\ultimate\mods\ExampleMod
```

---

# Troubleshooting

## ARCropolis text does not appear on startup

Check the following:

- `libarcropolis.nro` exists in:

```
sdmc\atmosphere\contents\01006A800016E000\romfs\skyline\plugins
```

- `subsdk9` exists in:

```
sdmc\atmosphere\contents\01006A800016E000\exefs
```

- Files were extracted to:

```
%AppData%\guarana\sdmc
```

---

## Mods are not loading

Make sure mods are installed inside:

```
sdmc\ultimate\mods
```

Each mod must have its **own subfolder**.

Correct example:

```
sdmc\ultimate\mods\ExampleMod
```

Incorrect example:

```
sdmc\ultimate\mods\ExampleMod\ExampleMod
```

---

## Installing mods in the wrong folder

ARCropolis mods **do not go in Eden's normal mod folder**.

Do **not** install Smash mods here:

```
user\load\01006A800016E000
```

That folder is only used for traditional **RomFS mods**, not ARCropolis.
