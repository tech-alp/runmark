# macOS CI ve release

İlk hedef macOS arm64. Linux sonra, Windows en son. Desktop, DMG,
Developer ID imzası, notarization ve updater bu aşamada yok.

## Ayrım

- `ci.yml`: PR/main üzerinde Debug build, CTest, module/QML deneyi,
  release-tool testleri/audit, Release paket ve hook testleri. Yayın yetkisi yok.
- `release.yml`: yalnız main üzerinde manuel `workflow_dispatch`.
  Varsayılan `publish=false` dry-run; `publish=true` gerçek yayın yapar.
- `release.config.cjs`: Conventional Commits analizi, notlar, CMake prepare,
  GitHub tag/release/artifact. npm yayını ve sürüm/changelog geri commit'i yok.
- `CMakePresets.json`: yerel ve CI derleme seçeneklerinin ortak kaynağı.
- CMake `install()` + BundleUtilities CLI runtime'ını yerleştirir; CPack TGZ
  üretir. Desktop geldiğinde Qt'nin QML deployment mekanizması ayrıca eklenir.

Referanslar: [Merce doğrulama/yayın ayrımı](https://github.com/tech-alp/Merce/blob/v1.2.0/.github/workflows/release.yml),
[Serial Studio paketleme](https://github.com/Serial-Studio/Serial-Studio/blob/master/app/CMakeLists.txt),
[semantic-release](https://semantic-release.org/usage/configuration/).
Kodları kopyalamak yerine Runmark'ın target sınırlarına uygun uygulandı.

## Toolchain

Qt 6.11.1, LLVM Clang 23.1.1, CMake 4.4.3, Ninja 1.13.2.
CI runner `macos-15`; minimum deployment target macOS 14.0. Minimum OS'ta
çalışma ayrıca ölçülmedi. Release araçları Node 24 CI üzerinde çalışır;
npm sürümleri `tools/release/package-lock.json` ile sabittir.

LLVM, resmi `llvmorg-23.1.1` macOS ARM64 arşivinden kurulur; URL ve SHA-256
`tools/ci/install-llvm.sh` içinde sabittir. Hash doğrulanmadan arşiv açılmaz.
`clang-scan-deps` aynı arşivden gelir. Kurulum runner geçici dizinindedir;
yerel Homebrew LLVM değiştirilmez. Arşiv yaklaşık 1.57 GB'dir.
CMake/Ninja hâlâ Homebrew'den gelir; `check-toolchain.sh` farklı sürümü
beklenen/gerçek sürüm mesajıyla reddeder. Bunların kurulumları henüz arşivle
sabitlenmiş değildir; sürüm değişiminde yeniden doğrulama gerekir.
Qt cache'i action tarafından yönetilir;
BMI/build dizinleri compiler'lar arasında cache'lenmez.

## Yerel doğrulama (yayın yapmaz)

```sh
export LLVM_ROOT=/opt/homebrew/opt/llvm
export QT6_ROOT="$HOME/Qt/6.11.1/macos"
bash tools/ci/check-toolchain.sh
npm --prefix tools/release ci --ignore-scripts
npm --prefix tools/release test
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
node tools/release/release.mjs prepare 0.3.1
```

Son komut temiz geçici build'de sürümü derlemeye aktarır; CTest ve hook
testlerinden sonra `build/artifacts/runmark-0.3.1-macos-arm64.tar.gz` ve
SHA-256 dosyasını üretir. Arşiv başka geçici dizine açılır; temiz environment
ile `--version`/fixture `inspect` çalışır. dyld çıktısı QtCore'un paketten
yüklendiğini, BundleUtilities bağımlılıkların paket içinde kaldığını doğrular.

`bin/` içeriğini birlikte taşı; yalnız `rmk` dosyasını kopyalama.
Ad-hoc imza yalnız arm64 çalıştırılabilirliği içindir; Gatekeeper/notarization
garantisi değildir. Geçici build/test dizinleri tanı için korunur.

## Sürüm politikası

- `fix`/`perf`: patch; `feat`: minor.
- `!` veya `BREAKING CHANGE:`: major; 0.x için de aynı kural.
- `docs`/`test`/`chore` tek başına release oluşturmaz.
- Tek kanal `main`, tag biçimi `vX.Y.Z`; ilk sürümde prerelease yok.
- Hesaplanan sürüm `-DRUNMARK_VERSION` ile CMake'e girer. Binary, arşiv ve tag
  aynı sürümdedir. Normal yerel configure varsayılanı 0.4.0'dır; son
  yayın sürümünün kaynağı repo içindeki bu varsayılan değil Git tag/release'dir.
- Agent plugin'i bu CLI arşivinden ayrı dağıtılır; `minimum_rmk_version`
  otomatik artırılmaz, uyumluluk değişikliğinde bilinçli güncellenir.

## İlk gerçek yayın öncesi gerekenler

2026-09-22 kapsam kararı: RM-5 CI/paketleme altyapısı olarak kapandı;
aşağıdaki yayın hazırlıkları RM-9 altında izlenir. Main `29c6c95` için
[CI 35736952337](https://github.com/tech-alp/runmark/actions/runs/35736952337)
tamamen geçti. Gerçek yayın yapılmadı. Kullanıcı lisansı erteledi ve geçmiş
yayın olmadığını doğruladı; ilk sürüm/bootstrap kararı henüz verilmedi.

1. Yayınlanacak main commit'i için hosted macOS CI sonucunu doğrula.
2. `release` environment kuruldu: yalnız main, gerekli onaylayıcı tech-alp,
   self-review açık. Admin bypass varsayılanı açık kaldı. Main branch
   protection'a macOS CI kontrolü henüz eklenmedi; YAML bunu kurmaz.
3. Runmark için lisans kararını ver ve köke `LICENSE` ekle. Kullanılan Qt ve
   diğer runtime bileşenlerinin tam lisans/bildirim metinlerini gözden geçirip
   `THIRD_PARTY_NOTICES/` içine koy. Otomasyon yalnız dosya varlığını doğrular;
   içerik doğruluğunun/lisans uyumluluğunun yerine geçmez.
4. İlk sürüm/bootstrap politikasını kararlaştır ve mevcut baseline gerektiren
   yayın kapısını bu karara göre test ederek düzenle. Geçmiş yayın yok;
   Yerel sürüm varsayılanı yayın kanıtı değildir. Tarihsel baseline uydurulmaz ve
   yanlışlıkla 1.0.0 başlatılmaz.
5. `publish=false` dry-run, sonra onaylı `publish=true` çalıştır.

Dry-run `prepare`/paketlemeyi çalıştırmaz; paket smoke testi CI'da ve gerçek
yayının prepare aşamasındadır. Dry-run da GitHub yetkisi ve yayın önkoşullarını
doğrular. Token yalnız semantic-release adımına verilir; `contents: write`
yalnız release job'undadır. Ek PAT veya npm token gerekmez.

İlk hosted CI (`35728139807`, `7d3aa96`) setup aşamasında başarısız oldu:
Homebrew LLVM 23.1.0 kurdu, sürüm kapısı 23.1.1 bekledi; build/test/paket
adımları atlandı. Sabit resmi arşiv kurulumu bu sapmayı giderdi. Sonraki
`35734931715` çalışmasında kurulum geçti; release testinin bare Git deposunda
varsayılan branch bağımlılığı bulundu ve explicit `main` ile düzeltildi.

[Hosted CI 35735808373](https://github.com/tech-alp/runmark/actions/runs/35735808373)
`fix/rm5-toolchain` branch'inde `82d8825` için tamamen geçti: toolchain,
release-tool testleri/audit, Debug build/test, module/QML köprüsü ve Release
paket smoke testi. Bu sonuç main'e entegrasyon veya gerçek yayın değildir.
GitHub release token akışı ve gerçek yayın doğrulanmadı.
Başarısız bir yayında mevcut tag'i silip yeniden yazma;
önce tag/release/artifact durumunu incele, düzeltmeyi yeni sürümle yayınla.
