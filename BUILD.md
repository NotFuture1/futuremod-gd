# Building Future Mod (macOS)

These steps take you from a fresh machine to a `.geode` file installed in
Geometry Dash. Your machine already has the Xcode Command Line Tools, so the
list below fills in the rest.

> Run every command in this folder unless told otherwise:
> `/Users/savabiktairov/Documents/Projects/futuremod-gd`

---

## 1. Install the prerequisites (one time)

You're missing **Homebrew packages + the Geode CLI + the Geode SDK**. If you
don't have Homebrew yet:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Then install CMake and the Geode CLI:

```bash
brew install cmake
brew install geode-sdk/geode/geode-cli
```

Verify:

```bash
cmake --version     # should be 3.21+
geode --version
```

---

## 2. Install the Geode SDK

This downloads the SDK source + prebuilt binaries and sets the `GEODE_SDK`
environment variable for you.

```bash
geode sdk install
geode sdk install-binaries
```

Close and reopen your terminal afterward so `GEODE_SDK` is loaded, then confirm:

```bash
echo $GEODE_SDK     # must print a path, not empty
```

If it's empty, add this to `~/.zshrc` (replace with the path `geode sdk install`
reported) and reopen the terminal:

```bash
export GEODE_SDK="$HOME/Documents/geode-sdk/geode"
```

---

## 3. Install Geode into Geometry Dash

The mod can't load without the Geode loader inside GD itself.

```bash
geode config setup     # point the CLI at your GeometryDash.app
```

Follow the prompt to install the Geode loader into that GD copy. Launch GD once
to confirm the Geode menu button appears, then quit.

---

## 4. Add a logo

Geode requires a `logo.png` in the project root to package the mod. Drop any
square PNG (ideally 336×336) here:

```
/Users/savabiktairov/Documents/Projects/futuremod-gd/logo.png
```

(Without it, `geode build` will error on packaging.)

---

## 5. Build + install the mod

From this folder:

```bash
geode build
```

That configures CMake, compiles, packages `swift.futuremod.geode`, and installs
it into your GD mods folder automatically.

To rebuild after editing `src/main.cpp`, just run `geode build` again.

---

## 6. Run it

1. Launch Geometry Dash.
2. Open **Geode → Installed** and confirm **Future Mod** is enabled.
3. (Optional) Open its settings ⚙️ to change the **Skip Key** (default Enter).
4. Finish a level and press the key — the Level Complete screen pops instantly.

---

## Manual build (without `geode build`)

If you prefer raw CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The packaged `.geode` ends up in `build/`. Copy it to your GD mods folder
(`<GD app>/geode/mods/`) or run `geode build` which does this for you.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Unable to find Geode SDK` | `GEODE_SDK` isn't set — redo step 2 / reopen terminal. |
| `geode: command not found` | `brew install geode-sdk/geode/geode-cli`. |
| Packaging fails on logo | Add `logo.png` (step 4). |
| Mod builds but doesn't load | Geode loader not installed into GD (step 3); check GD version matches `gd` in `mod.json` (2.2074). |
| Skip key does nothing | Confirm the mod is enabled in Geode → Installed; check the key code in settings. |
