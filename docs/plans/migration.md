# Runmark Mimari Geçiş Planı

Adlandırma geçişi (ROADMAP adım 1) kapandı. Kalan maddeler ROADMAP'teki
"Desktop öncesi — Adlandırma ve mimari geçişi" adımlarının task karşılığıdır.

Görevler `- [ ]` / `- [x]` checklist maddesi olarak yazılır ve task ID taşır.
Format `project.json` içindeki `task_id_pattern` ile eşleşmezse `rmk status`
`plan.no_parsable_tasks` uyarısı verir — kurallar sessizce kör kalmaz.

- [x] RM-1 — apps/cli ve libs sınırları

  `cli/` → `apps/cli/`; `core/` → `libs/domain`, `libs/application`,
  `libs/infrastructure`. CLI JSON/exit sözleşmesini ve mevcut veri
  sözleşmesini regression testleriyle koru.

- [x] RM-4 — Application tipli sonuç döndürsün

  `projectStatus` ve kardeşleri `QJsonObject` yerine struct döndürsün; JSON
  serileştirme `apps/cli`'ye insin. CLI sözleşmesi değişmez (TC-012).

- [x] RM-2 — Named module doğrulaması (macOS)

  Küçük bir named module ve QObject/QML köprüsünü macOS'ta
  clean ve incremental build ile doğrula; compiler/CMake/Ninja baseline'ını
  ölçüm sonrası sabitle.

  macOS deneyi geçti. Kullanıcı kararı (2026-09-22): önce macOS,
  ilerleyen süreçte Linux, en son Windows. Son iki platform RM-2/RM-3 kapısı değildir.
  Ölçülen macOS toolchain: LLVM Clang 23.1.1, CMake 4.4.3,
  Ninja 1.13.2, Qt 6.11.1; deney C++23 kullanır.
  Kaynak, komutlar ve sonuçlar: [module spike](../../tools/module-spike/README.md).

- [x] RM-3 — Domain'den başlayarak modules geçişi

  `runmark.domain` ile başla. Henüz kullanılmayan host/SDK dizinleri veya
  genel amaçlı framework soyutlamaları oluşturma.

  `runmark.domain` uygulandı; mevcut header yolları import köprüsüdür.
  Üretim macOS baseline'ı C++23 + LLVM + Ninja oldu.

- [x] RM-6 — Plan task kimliği tam token olmalı

  `plan.ambiguous_task_id`: pattern yalnız daha uzun bir kimliğin parçasına
  uyuyorsa satır hiçbir task'a bağlanmaz. Tahmini eşleme kanıtı yanlış task'a
  bağlıyordu.

- [x] RM-6 — Plan task kimliği tam token olmalı (kayıt)

  `plan.ambiguous_task_id`. Plana sonradan eklendi; kanıtlar
  `20260922T123129Z-RM-6` ledger'ında.

- [x] RM-7 — Desktop iskeleti: ui-shell ve salt okunur Findings

  `libs/ui-shell` (FindingModel + StatusViewModel, `Runmark.Shell` QML modülü)
  ve `apps/desktop`. Gerçek `projectStatus()` sonucunu gösterir, JSON parse
  etmez (TC-012). `apps/cli` GUI bağımlılığı kazanmamalı — mekanik kontrol.
  Merce bu adımda yok; düz Qt Quick Controls ile yapılır.

- [x] RM-8 — Merce entegrasyonu ve kompakt desktop profili

  TC-011'deki FetchContent bloğu, `Merce::*` hedefleri, statik linkleme.
  Sürüm SHA ile sabit. Kiosk profili değişmez.

  Merce v1.2.0 statik QML modülleri ve ayrı `desktop` profili uygulandı.
  Desktop 11/11, temiz CLI 10/10 test; QML lint ve kurulum smoke kontrolü geçti.
  Profil/paket sınırları: [ui-shell](../../libs/ui-shell/README.md).

- [x] RM-5 — macOS CI, paketleme ve manuel release altyapısı

  Sabit toolchain, build/test/hook gates, taşınabilir CLI arşivi ve paket
  smoke testi. Sürüm semantic-release tarafından build'e aktarılır.
  Yayın yalnız manuel main workflow'undan; Linux/Windows ve desktop imzalama sonra.

  Kullanıcı onayıyla kapsam 2026-09-22'de kapatıldı: CI ve paketleme tamamlandı,
  yayın yapılmadı. Main `29c6c95` için [hosted CI](https://github.com/tech-alp/runmark/actions/runs/35736952337)
  tamamen geçti. Release environment yalnız main, onaylayıcı tech-alp;
  self-review açık, admin bypass varsayılanı açık. Yayın hazırlığı RM-9'a taşındı.
  Ayrıntılar: [release rehberi](../RELEASING.md).

- [x] RM-10 — finish worktree durumunu bildirsin

  `finish` stdout'una `worktree` alanı: path, exists, clean, merged.
  `suggested_action` yalnız ikisi birden doğruyken çıkar; bilinmeyen durum
  `error` ile ayrılır. Runmark yine kendi silmez.

- [x] RM-11 — Ortak uyarı seti (cforgo değerlendirildi, alınmadı)

  `runmark::warnings`: `-Wall -Wextra -Wpedantic -Werror=return-type` ve
  out-of-source koruması. cforgo kurulup ölçüldü; değeri doğrulandı ama özel
  GitLab deposunda olduğu için public repoya bağımlılık olarak alınamadı
  (TC-011). cforgo yayımlanırsa dosya tek çağrıya döner.

- [x] RM-12 — Execution canlılığı: ikinci sinyal

  "Finish olayı yok" bir durum değeri; tek başına çalışan oturumla durmuş
  olanı ayıramıyor. Son kaydedilen olayın yaşı artık status bulgularında,
  resume paketinde ve markdown çıktısında. Ayrı heartbeat ve PID yok.

- [ ] RM-9 — İlk yayın hazırlığı ve release doğrulaması

  Main branch protection ve zorunlu macOS CI kontrolünü yapılandır.
  Lisans kararı kullanıcı tarafından ertelendi; LICENSE ve gözden geçirilmiş
  THIRD_PARTY_NOTICES yayın öncesi tamamlanmalı. Daha önce yayın yok;
  ilk sürüm/bootstrap politikası ayrıca kararlaştırılmalı, 0.3.0 kod sabitinden
  geçmiş yayın tag'i uydurulmamalı. Ardından onaylı dry-run ve gerçek release
  doğrulaması yap. Bu görev RM-8 desktop çalışmasını engellemez.

- [x] RM-13 — Faz 0a: süreklilik çekirdeği

  Proje bulma (alt klasör, worktree, proje listesi), oturum kaydı ve
  `rmk hook`, commit başına not hatırlatması, görevsiz not, çoklu plan
  dosyası, çakışma radarı, kayıtsız oturum taraması. Tasarım:
  `docs/designs/runmark-cockpit.md`, ADR-023.

- [ ] RM-14 — Faz 0a dogfooding: bir haftalık kullanım

  Runmark ve olympos'ta 0.4.0 ile gerçek iş. Oturum kayıtları çakışmayı,
  notsuz kapanışı ve kayıtsız oturumu sayar; yakalama günlüğü bunlardan
  tutulur. Codex hook'ları `/hooks` panelinden onaylanmalı.

  Pürüzler:
  - [x] #1 Açılış bağlamı en son execution'la başlıyor, son oturumun daha yeni
    notları altta kalıyordu. Notlar yeniyse artık önce onlar gösteriliyor.
  - [x] #2 Açılış yalnız en son execution'ı gösteriyordu; paralel işler
    görünmüyordu. Artık `## Open work` listesi var (ADR-024).
  - [x] #3 `interrupted` iş devam ettirilemiyordu (`rmk start` iki yoldan da
    reddediyordu). Artık `rmk start` onu devralıyor (ADR-024).
  - [ ] #4 Kapanmış bir execution'ın dalına `finish`'ten sonra gelen commit
    görünmüyor: RM-15'te `5e2714f` handoff'ta ve resume'da yok.
  - [x] #5 Paylaşılan checkout'ta commit hatırlatması commit'i atan oturuma
    değil klasördeki her canlı oturuma gidiyor; çakışma radarı da kirli
    dosyaları hepsine yazıyor. Codex oturumu Claude'un commit'leri için not
    yazdı (2026-09-27). Düzeldi: hatırlatmanın tabanı son prompt anındaki
    HEAD; radar aynı checkout için "paylaşıyor" der, dosyayı kimseye yazmaz.

- [x] RM-15 — Faz 0b öncesi: Nimbalyst incelemesi

  İş akışı dosyalarını okuyor mu, dışarıdan kaynak bağlanabiliyor mu (MCP,
  dosya)? Karar: Runmark kendi kokpitini mi büyütür, Nimbalyst'e motor mu olur.

  Karar (2026-09-27): kendi kokpiti. Kullanıcı Nimbalyst'i aktif kullanmıyor;
  paralel ajanları macOS'ta cmux, Linux'ta herdr ile yönetiyor. Rapor ve
  inceleme: `docs/designs/nimbalyst-review.md`.

- [ ] RM-16 — Faz 0b: herdr eklentisi (ADR-025)

  0. [x] herdr 0.9.1 kuruldu ve ölçüldü (2026-09-27):
     - Token gösterimi kullanıcı ayarı ister: `~/.config/herdr/config.toml`
       `[ui.sidebar.spaces] rows` içine `["$runmark"]`. Kenar çubuğu ~22
       karakterden sonrasını kesiyor; özet kısa olmalı.
     - Olaylar `workspace_id`, `pane_id`, `agent`, `agent_status`
       (`blocked|idle|working`) taşır; bağlam JSON'unda `workspace_cwd` var.
       Oturum kimliği yok.
     - herdr pane'lerinde `HERDR_WORKSPACE_ID`, `HERDR_BIN_PATH`,
       `HERDR_SOCKET_PATH` var; içindeki ajanın Runmark hook'u çalışma
       alanını bilir. Runmark hook'ları herdr içinde oturumu kaydetti.
  1. [x] Motor: `rmk init` (yeni projeyi kurar, var olanı yalnız
     `~/.config/runmark/projects.json`'a kaydeder), `rmk status --line`
     (`17/48 · 3 warn`; plan denetim bulguları sayılmaz) ve `status`
     JSON'unda `open_work`.
  2. Kenar çubuğu: açılışta ve olaylarda proje özeti.
  3. Eylemler: nerede kaldık, devam et, tüm projeler.
  4. Çözümü komut olan uyarılar eyleme bağlanır.
  5. Bir hafta kullanım; Qt desktop kararı.

  Kapsam dışı (kullanıcı kararı, 2026-09-27): makineler arası devam. Notlar,
  açık işler, oturumlar ve handoff'lar makineye yerel; yalnız
  `.runmark/project.json` git'le taşınır. Sonraki sürüm adayı.
