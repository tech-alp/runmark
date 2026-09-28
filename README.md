# Runmark

AI ile geliştirilen projelerde plan, kod, Git state ve agent context'in
birbirinden kopmasını engelleyen local-first execution control plane.

Doküman: [docs/](docs/README.md)

CI ve paketleme: [Release rehberi](docs/RELEASING.md).

Adlandırma uygulandı: binary `rmk`, veri dizini `.runmark/`, namespace
`runmark::`, Git ref'leri `refs/runmark/preserved/*`. CLI `apps/cli` altında;
katman ayrımı `libs/{domain,application,infrastructure}` olarak uygulandı.
Hedef düzen: [Architecture](docs/ARCHITECTURE.md).

## Derleme

Doğrulanan macOS arm64 toolchain: CMake 4.4.3, Ninja 1.13.2,
LLVM Clang 23.1.1, Qt 6.11.1 ve C++23. AppleClang yerine LLVM kullanılır.

```sh
export QT6_ROOT=~/Qt/6.11.1/macos      # kendi Qt yolun
export LLVM_ROOT=/opt/homebrew/opt/llvm
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

`QT6_ROOT` ayarlı değilse CMake Qt'yi sistem yollarında arar.
Kalıcı bir yapılandırma istersen `CMakeUserPresets.json` yaz (git ignore'da).

## Kullanım

```sh
rmk inspect                                   # project.json'u doğrula
rmk status                                    # findings üret
rmk start MF-1 --agent codex                  # verified worktree aç
rmk evidence <exec> --kind test --summary "…" # kanıt kaydet
rmk note <exec> --kind unresolved --text "…"  # açık madde kaydet
rmk finish <exec> --outcome finished          # ledger + handoff kapat
rmk resume [<task|exec>] --markdown           # bir sonraki oturumun bağlamı
```

Seçici verilmezse en son execution seçilir.

Durum `.runmark/` altında tutulur: `ledger/`, `evidence/`, `handoffs/`.
Şema: [docs/DATA_MODEL.md](docs/DATA_MODEL.md)

## Ajan eklentisi

`resume`'u oturum başında otomatik çağıran SessionStart hook'u
[plugins/runmark-agent](plugins/runmark-agent) altındadır. Hook CLI'yi
`PATH`'te arar, o yüzden önce kur:

```sh
cmake --install build/dev --prefix ~/.local    # ~/.local/bin PATH'te olmalı
claude plugin marketplace add .
claude plugin install runmark-agent@runmark
```

herdr kullanıyorsan [plugins/runmark-herdr](plugins/runmark-herdr) her çalışma
alanının altına `rmk status --line` özetini yazar (ADR-025):

```sh
herdr plugin link plugins/runmark-herdr
# ~/.config/herdr/config.toml
# [ui.sidebar.spaces]
# rows = [["state_icon", "workspace"], ["branch", "git_status"], ["$runmark"]]
```

Projenin `project.json`'una `"hooks_expected": true` yazıldığında hook hiç
çalışmamışsa `status` bunu `context.hooks_not_observed` olarak bildirir.
