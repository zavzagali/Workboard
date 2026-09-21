<blockquote>

## WorkBoard

  <b>All-in-one study app</b><br>
  Whiteboard, notes, pomodoro timer, tasks, and daily tasks — all in a single app.<br>
  
  # its currently under development
  

![CMake Build CI](https://github.com/zavzagali/WorkBoard/actions/workflows/build.yml/badge.svg)
![Latest Release](https://img.shields.io/github/v/release/zavzagali/WorkBoard?include_prereleases&color=green)
![Last Commit](https://img.shields.io/github/last-commit/zavzagali/WorkBoard)
![License](https://img.shields.io/github/license/zavzagali/WorkBoard?color=yellow)

</blockquote>

## Building

```bash
# Clone
git clone https://github.com/zavzagali/WorkBoard.git
cd WorkBoard

# Configure
cmake -Bbuild -GNinja

# Build
cmake --build build

# Run
./build/WorkBoard
```

### Install Dependencies (Arch Linux)

```bash
sudo pacman -S cmake ninja gcc pkg-config fontconfig freetype2 libxcb
```

### Install Dependencies (Ubuntu/Debian)

```bash
sudo apt install cmake ninja-build g++ pkg-config libfontconfig1-dev libfreetype6-dev libxcb-xfixes0-dev libxcb-render0-dev libxcb-shape0-dev libxcb-xkb-dev libxkbcommon-dev libssl-dev
```



