# Runmark Decisions

ADR-001–015, Runmark adıyla alınan karar geçmişidir; eski komut ve veri
yolları tarihsel bağlamıyla korunur. 22 Eylül 2026 tarihli ADR-016–019 hedef
mimariyi tanımlar. Kabul edilmiş hedef, kodun taşındığı anlamına gelmez.

## ADR-001 — Planning framework değil
Accepted.

Runmark Superpowers, planning-with-files vb. araçları entegre eder.

## ADR-002 — Evidence > agent claim
Accepted.

Commit, diff ve test; agent summary’den daha güçlü evidence’tır.

## ADR-003 — Plugin-first, not everything-is-a-plugin
Accepted.

Core yerleşimi ADR-018 ile güncellendi; ürün kavramlarının sahipliği korunur.

Core:
- ProjectGraph
- GitTruthEngine
- PlanTruthEngine
- ExecutionLedger
- EvidenceStore
- TrustEngine

## ADR-004 — v0.1 external plugin ABI yok
Accepted.

Önce provider contracts stabilize edilmeli.

## ADR-005 — Local-first
Accepted.

Cloud backend gerekmiyor.

## ADR-006 — Project anchor ve workspace root ayrıdır
Accepted.

## ADR-007 — Yeni iş verified remote base’den başlar
Accepted.

Default:

```text
git fetch
→ resolve remote base
→ health check
→ create worktree
```

## ADR-008 — MudIssue bir provider’dır
Accepted.

Runmark Mud’a bağımlı olmamalı.

## ADR-009 — Agent runtime external kalır
Accepted.

Claude/Codex auth Runmark’a taşınmaz.

## ADR-010 — Handoff durable source referanslamalı
Accepted.

## ADR-011 — Qt/QML desktop
Preferred.

Desktop seçimi ADR-017 ile kesinleşti; named modules yönü ADR-019'dadır.

```text
C++20
Qt 6.11+
QML
CMake
```

## ADR-012 — Trust explainable olmalı
Accepted.

Her warning somut evidence göstermeli.

## ADR-013 — Workspace core'da kalır, mevcut worktree sahiplenilir
Accepted.

Runmark workspace yaratabilir; MudIssue, IDE veya elle oluşturulmuş geçerli
worktree'yi de kullanabilir. Workspace'i kimin yarattığı dış mekanizmadır.

Runmark her durumda kendi ölçümünü kaydeder:

```text
fetch remote base
→ workspace/repo ilişkisini doğrula
→ dirty state kontrol et
→ HEAD ve merge-base'i ölç
→ ledger'a kaydet
```

MudIssue zorunlu bağımlılık değildir. v0.1'de workspace provider veya Mud
adapter'ı yoktur.

## ADR-014 — Ne bloklar, ne uyarır
Accepted.

Ana repo'nun kirliliği `start`'ı **engellemez**. Base uzak ref'ten çözüldüğü
için teknik bir engel yoktur; bu bir proje hijyeni tercihiydi ve bloklayıcı
olması insanları aracın dışına itiyordu.

Durum kaybolmaz: `execution.started` olayına `repo_dirty` yazılır ve `start`
çıktısı `warnings` dizisinde `git.dirty_workspace` döndürür.

Bloklayıcı kalanlar:

```text
worktree başka repo'nun   ölçüm yanlış repoyu ölçer
worktree başka execution'a ait
execution zaten var
task, task_id_pattern'e uymuyor
```

Worktree kirliliği artık bloklamaz. Runmark, working tree veya stash'e
dokunmadan geçici index ile snapshot commit'i üretir ve bunu
`refs/runmark/preserved/<exec-id>` altında saklar. Bu yakalama başarısızsa
`start`/`finish` başarısız olur; aksi halde kanıtsız iş üretirdi.

İlke: **ölçümü bozan şey bloklar, hijyen tercihi uyarır.**

---

## ADR-015 — Hook'un çalıştığı ölçülür, varsayılmaz
Accepted.

Kurulu sanılan ama hiç çalışmayan bir SessionStart hook'u, temiz bir projeden
ayırt edilemez: ikisinde de `status` sessizdir. Bu, projenin avladığı sessiz
körlük sınıfının aynısıdır.

`resume --hook` çağrıldığında `.runmark/hook-observed.json` yazılır. (Güncelleme
ADR-023: yerini oturum kayıtları aldı; herhangi bir kayıtlı oturum "gözlem var" demektir.) Üç sonuç:

```text
hooks_expected yok        → kural değerlendirilmez
beklenti var, gözlem yok  → context.hooks_not_observed (warning)
gözlem var                → sessiz
```

Beklenti `project.json`'da opt-in'dir. Her projede uyarmak, CLI'yi tek başına
kullanan projeye kapatamayacağı bir bulgu üretirdi — ADR-014'teki "hijyen
tercihi uyarır" ilkesi burada uyarının kendisini opt-in yapar.

Gözlem ledger olayı **değildir**. Ledger execution'a bağlı, append-only olay
kaydıdır; hook gözlemi hiçbir execution'a ait değil ve yalnız son değeri
anlamlı. Ayrıca `resume` bir okuma yoludur: oraya olay yazmak `resume`'u
idempotent olmaktan çıkarır ve ledger'ı ölçümle değil trafikle şişirirdi.

Yalnız "hiç görüldü mü" ölçülür. "En son ne zaman" tutulur ama henüz kural
üretmez; hook'un sonradan bozulduğunu yakalamak ayrı bir karardır.

## ADR-016 — Runmark adı, rmk CLI ve sorumluluk bazlı dizinler

Accepted. Adlandırma ve `apps/cli` + `libs/{domain,application,infrastructure}`
dizin ayrımı 2026-09-22'de uygulandı. Desktop ve runtime dizinleri ilgili
özelliklerle birlikte oluşturulacak.

Ürün adı Runmark, CLI executable adı `rmk` olur. Workspace verisi `.runmark/`
altında tutulur. Uygulamalar `apps/cli` ve `apps/desktop`; kütüphaneler
`libs/domain`, `libs/runtime` gibi sorumluluk adlarıyla yerleşir. Dizinlere
`runmark-` öneki eklenmez. Ürün kimliği `runmark::domain`, `runmark.domain`,
`Runmark.Shell`, `com.runmark.findings` gibi teknik namespace'lerde kullanılır.

Gerekçe: kısa terminal komutu ve ürün adına bağlı olmayan okunabilir dosya düzeni.

Bedeli birlikte taşınması gereken sözleşmelerdi ve hepsi aynı anda taşındı:
binary adı, `--version` çıktısının ilk kelimesi, agent plugin'inin
`minimum_rmk_version` anahtarı, hook'un çağırdığı komut, marketplace adları,
veri dizini ve preserved Git ref namespace'i. Biri atlanırsa hook sessizce
hiçbir bağlam enjekte etmez.

PoC sürecinde geriye uyumluluk aranmadı: eski ad, eski dizin ve eski ref'ler
korunmaz, migration kodu yazılmaz. Kurulu eski binary ve pluginler söküldü.

## ADR-017 — Desktop tasarım sistemi Merce

Accepted — hedef; entegrasyon bekliyor. Tarih: 2026-09-22.

Runmark desktop, Merce kullanır. Kompakt `desktop` profili hazırlanır; kiosk
profilleri değiştirilmez. Shell ve feature görünümleri ortak semantic tokenları
kullanır; QGravityUI ikinci bir runtime tasarım bağımlılığı olarak eklenmez.

Gerekçe: merkezi token/style yönetimi ve mevcut tasarım altyapısının kullanımı.
Veri ekranlarının model/view davranışları Runmark'ta geliştirilir.
Bedeli: Merce `Qt6::LabsStyleKit` (Technology Preview) ve Qt 6.11 ister, bu
yüzden Qt/Merce sürümleri sabitlenir; yükseltmede focus, tema, DPI ve veri
ekranları doğrulanır. Merce CLI/headless bağımlılığı olmaz.

Tüketim yolu TC-011'dedir: Merce ayrı bir depo (`tech-alp/Merce`) ve gömülmek
üzere tasarlanmış; referans tüketici `cart/app` submodule + `add_subdirectory`
kullanıyor.

## ADR-018 — İnce runtime, korumalı ürün servisleri, değiştirilebilir pluginler

Accepted — hedef; runtime henüz yok. Tarih: 2026-09-22.
ADR-003'ün core yerleşimini günceller; ADR-004'ün public native ABI sınırını korur.

Domain/application/infrastructure ürün davranışını; runtime ise manifest,
bağımlılık, lifecycle ve effect sahipliğini yönetir. Execution/evidence/trust
servisleri Runmark'a aittir ve korumalı system plugin üzerinden sunulabilir.
Üçüncü parti ürün servisi replacement sözleşmesi açılmaz. UI ve dış sistem
adaptörleri değiştirilebilir. Ekranı kaldırmak veriyi veya ürün servisini silmez.

Gerekçe: yeni ekran/entegrasyon eklerken doğrulama kuralları korunur; CLI ve
desktop aynı application akışlarını kullanır. Bedeli: activation rollback,
dependency kaybı, devam eden çağrılar ve effect cleanup test edilmelidir.
Process izolasyonu sandbox; resource cleanup harici işlem rollback'i sayılmaz.

## ADR-019 — Dahili C++ named modules, ayrı plugin sözleşmesi

Accepted — mimari yön; toolchain doğrulaması bekliyor. Tarih: 2026-09-22.

Dahili API sınırları C++ named modules ile ifade edilir; ilk adaylar
`runmark.domain`, `runmark.application`, `runmark.runtime` olur. CMake
`FILE_SET CXX_MODULES` kullanılır. QObject/QML köprüsü başlangıçta klasik
header/source dosyalarında kalır. BMI public plugin ABI'si değildir.

Gerekçe: açık export yüzeyi ve döngüsüz bağımlılıklar. Bedeli: compiler/BMI,
dependency scanning ve MOC entegrasyonu; build hızlanması garanti değildir.
CMake tabanı 4.4'e çekildi — `FILE_SET CXX_MODULES` 3.28'de geldi, Merce v1.1.0
3.30 ister ve 4.4 yerel derlemede doğrulanan sürümdür. C++23, Ninja ve LLVM Clang/MSVC kombinasyonu adaydır; macOS, Linux ve
Windows clean/incremental build doğrulaması sonrası seçilir.
Mevcut C++20 build değiştirilmedi. `import std`, header units, Conan ve özel
MOC helper'ı bu kararla otomatik eklenmez.

---

## ADR-020 — Orkestrasyon kalıpları ertelendi, sırası yazıldı
Accepted — erteleme kararı. Tarih: 2026-09-22.

CAO'nun (`awslabs/cli-agent-orchestrator`) üç orkestrasyon kalıbı incelendi:

```text
Assign        ajanı başlat, arka planda bırak, sonra rapor al
Handoff       ajanı başlat, bitmesini bekle, çıktısını yakala
Send Message  çalışan iki ajan arasında doğrudan kanal
```

Üçü de alınmadı. Gerekçe "gereksiz" değil: ARCHITECTURE'ın agent start akışı
zaten `launch agent` ile bitiyor ve bileşen listesinde `Process Host` var.
Kalıplar mimaride yeri ayrılmış işlerdir; **ön koşulları yok.**

Bugün `rmk start` worktree'yi açar, bağlam paketini üretir ve durur; ajanı
insan başlatır. Başlatma yolu yokken "senkron devret" yazmak, olmayan bir şeyi
devretmek olurdu. Sıra şudur ve atlanamaz:

```text
1. launch agent    provider adaptörü, süreç gözetimi, çıkış/kesinti tespiti
2. assign          1 olmadan anlamsız
3. handoff         1 + bekleme + çıktı yakalama
4. send_message    1 + iki canlı süreç
```

Bedeli ölçüldü: CAO'da bu üç satırlık API'yi `providers/` 864 KB (20 ajan
CLI adaptörü), `backends/` 80 KB (tmux/herdr terminal soyutlaması) ve
`services/` 2.1 MB taşıyor. Ve `handoff`'un yakaladığı çıktı bir **terminal
ekranından** okunuyor; CAO'nun kendi belgesi "There is no structured protocol
between CAO and the provider CLI" diyor ve `cao-session-liveness` skill'i tam
bu yüzden var. O yola girmek, elimizdeki üstünlüğü satın alınan bir zayıflıkla
değişmek olurdu: bizde ajan CLI'yi kendisi çağırıyor, sinyal yapısal.

Bu ailenin en küçük ve en sağlam üyesi RM-12'de alındı: eşzamanlı bir kanal
değil, asenkron bir kayıt. Bir ajan `note` yazar, diğeri `resume`'da onu ve
son etkinlik yaşını görür. Yavaş, ama ekran okumuyor.

**İsim çakışması.** Runmark'ın "handoff"u CAO'nunkiyle aynı şey değildir:
bizimki `finish` anında yazılan bir **belge** (DATA_MODEL §6), CAO'nunki
**bloklayan bir çağrı**. Aynı kelime, iki kavram; karışırsa yanlış beklenti
üretir.

Yeniden değerlendirme tetikleyicisi: `launch agent` uygulandığında. Ondan önce
bu kalıplar gündeme alınmaz.

## ADR-021 — Test kanıtı ajanın sözünden değil, runtime transcript'inden
Accepted. Tarih: 2026-09-25.

`rmk evidence --kind test` ajanın yazdığı metni kaydediyordu; Runmark komutu
çalıştırmıyordu. Buna rağmen olay `plan.done_without_evidence`'ı kapatıyor ve
resume paketinde `measured` altında görünüyordu: ajan kendi ödevini notluyordu.

Karar: `start`, ajan runtime'ının oturum kimliğini kaydeder; `finish`, o
oturumun transcript'inden (Claude `~/.claude/projects`, Codex
`~/.codex/sessions`) start'tan sonraki test koşularını ve çıkış kodlarını okur,
`source: runtime` olarak yazar. Ajanın yazdığı test kanıtı `source: agent`
kalır ve beyandır.

Neden launch değil: ajanı başlatıp akışını okumak (Multica `pkg/agent`) aynı
sinyali canlı verir ama Go araç zinciri, yeni süreç ve lisans kararı ister.
Transcript aynı sinyali bugün, sıfır yeni bağımlılıkla verir. Teknik TeamAI'ın
`transcript-parser.ts`'inden: modele sormadan araç çağrısından sinyal çıkarmak.

Sınırlar: transcript biçimleri belgelenmemiştir; tanınmayan biçim `unavailable`
olur, temiz değil. Aynı oturumdan eşzamanlı iki execution test koşularını
paylaşır. Shell erişimli ajan dosyayı teoride değiştirebilir; runtime kaydı
Runmark'ın kendi ölçümünden zayıftır.

Yeniden değerlendirme tetikleyicisi: bir runtime transcript biçimini değiştirdiğinde
veya `launch agent` uygulandığında.

Ek (2026-09-25): İki açık kapatıldı. (1) Pipe: `ctest | tail` gibi komutlarda
çıkış kodu sonraki komutundur; bu depodaki oturumlarda test komutlarının
135/142'si böyleydi. Bu durumda kod `null` yazılır ve
`context.test_result_unknown` bulgusu çıkar; bilinmeyen sonuç geçmiş sayılmaz.
(2) Ajanın kendi kaydettiği hiçbir kanıt (`commit`, `test`, `agent_summary`…)
artık `plan.done_without_evidence`'ı kapatmaz; yalnız execution commit'i ve
runtime kanıtı kapatır.

## ADR-022 — Kanıt kaynağı politikası: kurallar bizim, kaynaklar değiştirilebilir
Accepted. Tarih: 2026-09-25.

Soru: Entire, Multica gibi ajan oturumunu kaydeden araçları Runmark okumalı mı,
ve bu tür araçların hepsine destek verecek miyiz?

Ölçüm (Entire CLI 0.11.2, sandbox repo, telemetri kapalı): Entire her commit'e
`Entire-Checkpoint` trailer'ı ekler, oturumun transcript'ini
`refs/entire/checkpoints/<shard>/<id>` altında `0/full.jsonl` olarak saklar.
Bu dosya Claude ve Codex'in kendi transcript'inin kopyasıdır; Runmark'ın
ayrıştırıcısı olduğu gibi okur. Üç senaryo:

- "tests pass" mesajlı commit, exit 1 ile biten teste bağlandı; Entire kaydetti,
  uyarmadı. Kaydeder, yargılamaz.
- Codex'in gerçek düzeltmesi: checkpoint'te her komut `exit_code` ile.
- Codex proje hook'ları onaylanmadan: Entire hook'ları çalışmadı, commit
  checkpoint'siz kaldı; commit anında uyarı yok, yalnız `entire status` /
  `doctor` sorulunca söylüyor.

Karar:

1. **Kurallar kaynak bilmez.** Domain yalnız tipli olayları görür (B1);
   bir kaynağı okumak infrastructure'da bir okuyucudur.
2. **Her zaman okunan:** Git ve desteklediğimiz ajan runtime'larının kendi
   transcript'leri (Claude, Codex). Runtime'ı destekliyorsak biçimine zaten
   bağımlıyız.
3. **Okunmayan:** üçüncü taraf kaydediciler (Entire, Multica, …). Entire yeni
   veri getirmez, aynı verinin başka konumunu getirir; karşılığında dosya düzeni,
   hedefi platform olan bir şirketin kontrolüne girer. Runmark hiçbir üçüncü
   taraf araç kurulu olmadan tam çalışır.
4. **Yeni kaynak için üç şartın üçü de:** başka yoldan alınamayan bir kanıt
   getirir; gerçek bir kullanıcı onu kullanıyor; biçim tanınmazsa sonuç
   "bilinmiyor" olur, "temiz" değil.
5. **Runtime biçimi testle korunur:** `libs/infrastructure/tests/transcripts/`
   altında gerçek runtime'ların ürettiği, izin listesiyle temizlenmiş
   (`sanitize.py`) örnekler durur; dizin adı üreten sürümdür
   (`claude-2.1.282`, `codex-0.156.1`). Biçim değişirse test kırılır, kullanıcının
   kanıtı değil. Sürüm izin listesi tutulmaz: runtime'lar haftalık sürüm çıkarır,
   her yeni sürüm bulgu üretseydi gürültü olurdu; sinyal biçimin tanınmasıdır
   (`context.transcript_unavailable`).

Kanıtın makineler arası taşınması Entire'ı gerektirmez: `finish` test koşularını
zaten `.runmark/evidence/` altına kendi biçimiyle yazar. Taşınma, `.runmark`
durumunun paylaşılıp paylaşılmayacağı kararıdır; ayrı konu.

Yeniden değerlendirme tetikleyicileri: bir kullanıcı makineler arası kanıt
ister ve Entire kullanıyordur; bir runtime yerel transcript yazmayı bırakır;
Entire checkpoint biçimini belgelenmiş ve sürümlü bir sözleşme olarak yayınlar.

## ADR-023 — Oturum kaydı hook'la, karar hatırlatması commit başına
Accepted. Tarih: 2026-09-26. Kaynak: `docs/designs/runmark-cockpit.md` Faz 0a.

Ajanlar çoğunlukla `rmk start` denmeden açılıyor ve oturum kapanınca kararları
kayboluyordu. Karar:

- Her ajan oturumu, nasıl başlatılırsa başlatılsın, plugin hook'larıyla
  `.runmark/sessions/<session_id>.jsonl` dosyasına yazılır (açıldı, çalışıyor,
  bekliyor, hatırlatıldı, kapandı). `hook-observed.json` kaldırıldı.
- Tek `rmk hook <olay>` komutu hook JSON'unu stdin'den okur; tüm karar C++'ta,
  plugin yalnız sürüm kontrolü yapıp iletir (kayıt biçimi yalnız `ledger.cpp`'de).
- `rmk` projeyi üst dizinlerde, worktree'nin ana checkout'unda ve
  `~/.config/runmark/projects.json` listesinde arar; worktree'de açılan hook'lar
  eskiden projeyi bulamayıp sessizce çıkıyordu.
- Stop hook'u, oturumda yeni bir commit varken ve o commit'ten sonra not yokken
  commit başına bir kez ajanı `rmk note` yazmaya zorlar (`block`); `stop_hook_active`
  iken asla. Block davranışı Claude 2.1.282 ve Codex 0.156.1'de ölçüldü. Yedek:
  sonraki oturum açılışı notsuz kalan oturumu söyler.
- Stop yolu < 100 ms (ölçülen 18 ms): ağ yok, transcript okunmaz.
- `rmk note` execution'sız çağrılırsa not oturumun dosyasına gider.
- SessionStart `startup|resume|clear|compact` ile eşleşir (plugin 0.4.1).
  Ölçüldü, Claude 2.1.282 ve Codex 0.156.1: `/clear` yeni `session_id` ve yeni
  transcript ile `source=clear` açar; `/compact` aynı oturumda `source=compact`
  açar. Claude eski oturumu `/clear` anında `reason=clear` ile kapatır; Codex
  kapanışı ~1 dk gecikmeyle ve her durumda `reason=other` ile gönderir.

Sınırlar: hook'u hiç çalışmayan oturum bu yolla görünmez (transcript taraması
T6 bunu kapatır). Oturum ana dizinde açılıp başka bir worktree'ye elle
geçildiyse, o worktree'nin commit'leri yalnız oturum `rmk start` ile bir
execution başlattıysa görülür.

## ADR-024 — Yarım kalan iş devam ettirilir, açılışta hepsi görünür
Accepted. Tarih: 2026-09-26. Kaynak: RM-14 dogfooding.

`interrupted` ile bırakılan bir iş hiçbir yoldan devam ettirilemiyordu:
worktree yerindeyse `rmk start` "worktree already belongs" diye, silinmişse
git "branch already exists" diye reddediyordu. Oturum açılışı da yalnız en son
execution'ı gösteriyordu; paralel işler görünmüyordu. Karar:

- Açık iş: her görevin en yeni execution'ı, hiç bitmemişse ya da `interrupted`
  bittiyse. Tek tanım domain'deki `openExecutions()`; status, start ve oturum
  açılışı onu kullanır.
- `rmk start <görev>`, görevin açık execution'ı `interrupted` ise onun branch'ini
  ve worktree'sini devralır (`adopted`); worktree silinmişse branch'ten yeniden
  kurar. Hiç bitmemişse reddeder ve `rmk resume <exec>` ya da `rmk finish`
  önerir.
- Status `interrupted` işi `context.interrupted_execution` ile gösterir; onun
  worktree'si için "sil" önermez.
- Oturum açılışı, resume edilen dışındaki açık işleri en üstte `## Open work`
  olarak listeler.

Sınır: `finished` ya da `abandoned` bir görevi yeniden açmak hâlâ elle yapılır.
