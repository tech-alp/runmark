# Runmark Data Model (mevcut v0.1 sözleşmesi)

Ürün Runmark olarak adlandırıldı; bu doküman henüz kodda kullanılan
`rmk`, `.runmark/` ve `refs/runmark/preserved/*` sözleşmelerini tanımlar.
Örnek çıktılardaki Runmark adı mevcut üreticiye aittir. Hedef CLI `rmk`,
veri dizini `.runmark/` olacaktır; otomatik migration henüz yoktur.
Eski/yeni dizin birlikte bulunduğunda seçim ve rollback davranışı ayrıca
tanımlanacaktır. Adlandırma değişikliği kayıt şemasını kendiliğinden değiştirmez.
Bkz. [Architecture](ARCHITECTURE.md) ve [ADR-016](DECISIONS.md#adr-016--runmark-adı-rmk-cli-ve-sorumluluk-bazlı-dizinler).

Bu doküman `start` / `finish` / `status` / `resume` komutlarının yazdığı ve okuduğu verileri tanımlar.
Kod değil, şema. Amaç: C++ yazmadan önce "ne nereye yazılıyor" sorusunun bitmesi.

Kapsam: tek repo, tek plan dosyası, provider interface yok.
Genişleme noktaları en altta işaretli.

---

## 1. Disk düzeni

```text
.runmark/
├ project.json                                  el ile yazılır
├ ledger/20260918T142231Z-SCMS-042.jsonl        execution başına 1 dosya
├ handoffs/20260918T142231Z-SCMS-042.md         execution başına 1 dosya
├ evidence/<sha1>.json                          büyük payload'lar
└ sessions/<session_id>.jsonl                   ajan oturumu başına 1 dosya (hook'lar yazar)
```

**Execution başına ayrı dosya** — iki agent aynı anda iki worktree'de çalışırken
yazma çakışması olmaz. Kilit yok, lock dosyası yok.
Dosya adı zaman damgasıyla başladığı için `ls` çıktısı zaten kronolojik;
ayrı index'e gerek yok.

`cache/` ve `index/` v0.1'de yok.
Ledger birkaç yüz satır; her `status`'ta baştan okunur.
`status` 200 ms'yi geçerse index eklenir.

**Oturum başına ayrı dosya** (`sessions/`) — ajan oturumlarının çoğu bir
execution'a bağlı değildir (`rmk start` denmeden açılır), bu yüzden ledger'da
sayfaları yoktur. Her oturumu hook'ları yazar; tek yazar olduğu için kilit yok.
Olaylar (`ts` milisaniyeli, `session` alanlı):

| type | Alanlar | Kim yazar |
|---|---|---|
| `session.started` | `runtime` (claude/codex, transcript yolundan), `cwd`, `transcript`, `source`, `head` (başlangıç HEAD'i) | `rmk hook session-start` |
| `session.working` | — | `rmk hook prompt-submit` |
| `session.waiting` | — (ajan cevabını bitirdi, kullanıcıyı bekliyor) | `rmk hook stop` |
| `session.reminded` | `head` (not istenen commit) | `rmk hook stop` |
| `session.ended` | `reason` | `rmk hook session-end` |
| `note` | §3.4 ile aynı; `exec` null | `rmk note` (exec verilmeden) |

Oturum yeniden açılırsa (`source: resume`) ilk `head` taban kalır. `rmk status` kapanmamış ve son 12 saatte
hareket görmüş iki oturumun aynı göreve (başlattıkları execution'lar) ya da aynı
dosyalara (tabanlarına göre `git diff`, commit'lenmemiş değişiklik dahil; anahtar
`<git common dir>//<yol>`) dokunduğunu görürse `context.session_conflict` üretir.

Hook'lar kendi yokluklarını bildiremez, ama runtime'lar transcript'i her durumda
yazar. `rmk status` son 3 günün Claude (`<config>/projects/*/*.jsonl`, ilk `cwd`)
ve Codex (`<home>/sessions/**/rollout-*.jsonl`, `session_meta`) transcript'lerini
tarar; çalışma dizini proje kökünde, worktree kökünde ya da bir repoda olup
`sessions/`'ta kaydı olmayan oturumlar `hooks_expected` açıkken
`context.unregistered_session` üretir. `parent_thread_id` taşıyan Codex alt
thread'leri sayılmaz. Silinmiş bir worktree'deki oturum da sayılır. Herhangi bir
oturum kaydı, hook'un çalıştığının kanıtıdır; eskiden bunu tutan
`hook-observed.json` kaldırıldı (eng review D5).

Project anchor bir Git worktree içindeyse Runmark yalnız ürettiği `ledger/`,
`evidence/`, `handoffs/` ve `sessions/` yollarını ortak Git dizinindeki `info/exclude`a
idempotent ekler. `.runmark/project.json` dışlanmaz; proje config'i takip
edilir. Paylaşılan `.gitignore` Runmark tarafından değiştirilmez.

**Execution ID formatı:**

```text
<UTC ISO8601 compact>-<task_id>
20260918T142231Z-SCMS-042
```

Sıralanabilir, okunabilir, kütüphane gerektirmez.

---

## 2. project.json

Format kararı: TECH_CHOICES.md TC-003 (YAML yerine JSON → Qt dışı sıfır bağımlılık).

```json
{
  "version": 1,
  "name": "scms",

  "worktree_root": "~/worktrees",

  "repos": [
    {
      "name": "scms-core",
      "path": "~/projects/scms-core",
      "base": {"remote": "origin", "branch": "development"}
    }
  ],

  "plan": { "paths": ["docs/plans/config-migration.md", "docs/superpowers/plans/*.md"] },

  "task_id_pattern": "SCMS-\\d+",
  "instructions": ["AGENTS.md", "docs/WORKFLOW.md"],
  "hooks_expected": true,
  "test_command_pattern": "\\b(ctest|pytest)\\b"
}
```

- `worktree_root` — ADR-006: project anchor != workspace root
- `repos[].base.remote` + `repos[].base.branch` — ADR-007: verified remote
  base. Git ref'i `remote + "/" + branch` olarak türetilir; eski
  `"origin/main"` string biçimi kabul edilmez.
- `plan.paths` — boş olmayan plan dosyası listesi; dosya adında glob olabilir
  (`docs/superpowers/plans/*.md`). Superpowers, planning-with-files ve GSD
  ilerlemeyi `- [ ]` ile yazar; tek okuyucu hepsini okur. `rmk status` dosya başına
  `plans: [{path, done, total}]` verir. Okunamayan dosya ya da hiçbir şeye uymayan
  glob ayrı `plan.unreadable` bulgusudur; diğer dosyalar okunmaya devam eder.
  Execution'ın `plan_sha1`'i görevin geçtiği dosyanındır (`plan_ref`'in dosyası);
  görev hiçbir planda yoksa karşılaştırma "bilinmiyor"dur.
- `task_id_pattern` — Plan ↔ evidence eşleşmesinin dayandığı konvansiyon. Bu
  pattern olmadan PlanTruthEngine çalışmaz.
- `hooks_expected` — opsiyonel boolean, varsayılanı `false`. Proje bir ajan
  eklentisi kurduysa `true` yazılır; ancak o zaman `context.hooks_not_observed`
  değerlendirilir. Beklenti yazılmadan uyarmak, CLI'yi tek başına kullanan
  projeye kapatamayacağı bir bulgu üretirdi.
- `test_command_pattern` — opsiyonel regex. Ajan transcript'indeki hangi
  komutların test koşusu sayılacağını seçer (§3.3). Verilmezse yaygın
  koşucuları (`ctest`, `pytest`, `go test`, `cargo test`, `npm test`, …) kapsayan
  varsayılan kullanılır; varsayılan `project.json`'a yazılmaz.

v0.1'de `repos` tek elemanlı. Liste olması multi-repo'yu şema değiştirmeden açar.

`instructions` opsiyonel bir yol dizisidir; verilmezse `[]`. Elemanlar boş
olmayan string olmalıdır. `start --instruction <yol>` tekrarlanabilir ve
proje listesinin sonuna sırayla eklenir. Göreli yollar diğer proje yolları
gibi project anchor'a göre çözülür; `~` genişletilir. İçerik çalıştırılmaz,
skill keşfi veya yönetimi yapılmaz; yalnız dosya adı, mutlak yol ve SHA1 kaydedilir.

---

## 3. Ledger — JSONL, append-only

Her satır bir olay. Dört tip var, fazlası yok.

Anahtar isimlerini yalnız `libs/infrastructure/src/ledger.cpp` bilir: satırları
domain'deki tipli olaylara (`ExecutionStarted`, `ExecutionFinished`,
`EvidenceRecorded`, `NoteRecorded`) çevirir ve yazarken geri çevirir. Kurallar,
application ve CLI yalnız bu tipleri görür. Bilinmeyen `type` taşıyan satır
atlanmaz; `context.unrecognised_ledger_event` bulgusu olur — daha yeni veya
bozuk bir ledger, daha sessiz bir ledger gibi okunmamalı. JSON olmayan satır
okumayı durdurur.

Ortak alanlar: `ts` (UTC ISO8601), `type`, `exec`.

### 3.1 execution.started

`rmk start <task>` preflight'ı geçtikten sonra yazar.

```json
{
  "ts": "2026-09-18T14:22:31Z",
  "type": "execution.started",
  "exec": "20260918T142231Z-SCMS-042",
  "task": "SCMS-042",
  "agent": "codex",
  "repo": "scms-core",
  "worktree": "~/worktrees/SCMS-042",
  "branch": "task/SCMS-042",
  "workspace_source": "created",
  "repo_dirty": false,
  "preserved_ref": null,
  "base": "origin/development",
  "base_sha": "a1b2c3d4",
  "remote_base_sha": "a1b2c3d4",
  "head_sha": "a1b2c3d4",
  "plan_ref": "docs/plans/config-migration.md#t8",
  "plan_sha1": "<plan dosyasının 40 haneli SHA1'i>",
  "instructions": [
    {"name": "AGENTS.md", "path": "/projects/scms/AGENTS.md", "sha1": "<40 haneli SHA1>"},
    {"name": "WORKFLOW.md", "path": "/projects/scms/docs/WORKFLOW.md", "sha1": null}
  ]
}
```

`plan_ref` boş olabilir → `plan.execution_without_plan_link` finding'i doğar.

`session_id`: `start`'ı çalıştıran ajan runtime'ının oturum kimliği; runtime
dışından çalıştırıldıysa `null`. `agent` alanına göre okunur: `claude` →
`CLAUDE_CODE_SESSION_ID`, `codex` → `CODEX_THREAD_ID`. Yalnız `[A-Za-z0-9-]`
kabul edilir; kimlik dosya adına girer. `finish` bu oturumun transcript'ini okur.

`repo_dirty`: start anında ana repo'nun kirli olup olmadığı (ADR-014).
Bloklamaz, kaydedilir ve `start` çıktısının `warnings` dizisinde döner.

`preserved_ref`: worktree'de commit edilmemiş iş varsa
`refs/runmark/preserved/<exec>`; yoksa `null`. Ref, geçici index ile üretilen
snapshot commit'ini gösterir; working tree ve stash değiştirilmez.

`workspace_source`: `created` | `adopted`. `created` için `base_sha`, fetch
sonrası remote base'dir. `adopted` için Runmark'un ölçtüğü
`merge-base(HEAD, base)` değeridir; dış aracın "hangi SHA'dan açtım" iddiası
ledger'a yazılmaz.

`remote_base_sha`: start'ın fetch sonrasında ölçtüğü remote uç SHA'sı.
Adopted worktree'nin `base_sha` alanından farklı olabilir: `resume` başlangıçtan
beri ilerlemeyi bu alanla karşılaştırır; önceden var olan geriliği yeni ilerleme
saymaz. Eski `created` kayıtlarında `base_sha` kullanılabilir; eski `adopted`
kayıtlarında başlangıç remote SHA'sı bilinmiyorsa karşılaştırma `null` olur.

`instructions`: her yeni start'ta dizi; talimat yoksa `[]`. Dosya okunamıyorsa
eleman atılmaz: `sha1: null` kaydedilir ve start sonucunun `warnings` dizisine
`context.instruction_unreadable` finding'i eklenir. SHA1 dosyanın ham byte'ları
üzerinden hesaplanır. Eski olayda alanın bulunmaması, talimat olmadığını kanıtlamaz.

### 3.2 execution.finished

`rmk finish` yazar.

`finish` **stdout'una** ayrıca `worktree` alanı koyar; bu ledger'a yazılmaz,
yalnızca çağırana o anki durumu bildirir:

```json
"worktree": {
  "path": "/worktrees/SCMS-042",
  "exists": true,
  "clean": true,
  "merged": true,
  "suggested_action": "git worktree remove /worktrees/SCMS-042"
}
```

`suggested_action` **yalnız** `clean` ve `merged` birlikte doğruyken çıkar.
Kirli veya merge edilmemiş bir worktree için komut vermek, iş kaybetmenin
yolunu uzatmak yerine kısaltırdı. Kontrollerden biri hiç çalışamazsa
`error` dolar ve `clean`/`merged` hiç yazılmaz — bilinmeyen, güvenliyle
karıştırılmaz.

Runmark worktree'yi kendi silmez (ARCHITECTURE "Güvenlik"). Bu alan, ayakta
duran `git.orphaned_worktree` bulgusunu kimsenin araması gerekmesin diye
kararın verildiği ana taşır.

```json
{
  "ts": "2026-09-18T16:04:02Z",
  "type": "execution.finished",
  "exec": "20260918T142231Z-SCMS-042",
  "outcome": "finished",
  "head_sha": "f9e8d7c6",
  "commits": ["b2c3d4e", "f9e8d7c"],
  "files_changed": 12,
  "insertions": 340,
  "deletions": 58,
  "files_ref": "evidence/3a7f91c2.json",
  "preserved_ref": null,
  "handoff_sha1": "<üretilen handoff byte'larının 40 haneli SHA1'i>",
  "transcript": {"status": "read", "path": "<transcript yolu>", "commands": 14, "test_runs": 2}
}
```

`transcript.status`: `read` | `unavailable` (kimlik var, dosya bulunamadı veya
biçim tanınmadı; `error` dolar) | `no_session` (start runtime dışından). Yalnız
`start`'tan sonraki komutlar sayılır. `unavailable`, "test koşmadı" demek değildir:
`context.transcript_unavailable` bulgusu üretir.

`outcome`: `finished` | `interrupted` | `abandoned`

Satırı küçük tutmak için değişen dosya listesi `evidence/` altına ayrı yazılır.
`handoff_sha1`, finish'in ürettiği dosyayı sonradan okunan dosyayla karşılaştırır.
Eski finish kayıtlarında bulunmayabilir; resume bu durumda doğrulandığını iddia etmez.

### 3.3 evidence.recorded

```json
{
  "ts": "2026-09-18T15:40:11Z",
  "type": "evidence.recorded",
  "exec": "20260918T142231Z-SCMS-042",
  "task": "SCMS-042",
  "kind": "test",
  "source": "runtime",
  "runtime": "claude",
  "exit_code": 0,
  "ref": "evidence/8c1d0b44.json",
  "summary": "2 test run(s); last: ctest --preset dev (exit 0)"
}
```

`source` kanıtın kimden geldiğini söyler (ADR-021):

- `runtime` — `finish`, ajan runtime'ının transcript'inde bulduğu test
  koşularından yazar. Execution başına en fazla bir olay; `exit_code` son
  koşunun kodu, `ref` tüm koşuların listesi. Claude'da başarısız komut
  `Exit code N` taşır, başarılı komut kod taşımaz (0 sayılır); kodsuz hata `-1`.
  Komutun çıkış kodu testin kendisine ait değilse `exit_code: null` yazılır:
  test komutundan sonra `|`, `;`, `||`, satır sonu veya arka plan `&` gelirse
  kod sonraki komutundur (`ctest | tail` test patlasa da 0 döner). `&&` ve
  `set -o pipefail` altındaki pipe kodu korur. Bu depodaki ajan oturumlarında
  142 test komutunun 135'i pipe'lıydı; bilinmeyen sonuç geçmiş sayılmaz.
- `agent` — `rmk evidence` ile ajanın kendisi yazar. Alanı olmayan eski kayıtlar
  da `agent` sayılır. Aynı `kind` olsa da **beyandır**: resume paketinde
  `agent_claims` altında durur, `plan.test_claim_unverified` bulgusunu kapatmaz
  ve `plan.done_without_evidence`'ı da kapatmaz. "Bitti"yi yalnız ölçülen kapatır:
  execution'ın commit'i veya runtime kanıtı.

Güç sırası: ajanın sözü < runtime kaydı < Runmark'ın kendi ölçtüğü (Git).
Runtime kaydı modelin uyduramayacağı ama shell erişimli bir ajanın teoride
değiştirebileceği bir dosyadır.

`kind` TRUST_MODEL.md'deki hiyerarşinin tam karşılığı:

```text
commit        (en güçlü)
diff
test
files
command
agent_summary
manual_note   (en zayıf)
```

Güçlülük derecesi **saklanmaz**, `kind`'dan türetilir.
Tek yerde tanımlı sabit tablo; ledger'a yazılırsa iki kaynak doğar.

### 3.4 note

Karar ve açık maddeler.

```json
{
  "ts": "2026-09-18T15:12:00Z",
  "type": "note",
  "exec": "20260918T142231Z-SCMS-042",
  "kind": "unresolved",
  "text": "Config migration'da eski format okuma desteği kalacak mı?",
  "source": "agent",
  "ref": null,
  "session": "06a72e06-b1ae-49ab-83a5-de70513b6222"
}
```

`session`: notu yazan ajan oturumu (`CODEX_THREAD_ID`, yoksa `CLAUDE_CODE_SESSION_ID`);
oturum dışından yazıldıysa `null`. `rmk note` execution ID'siz çağrılırsa not
oturumun kendi dosyasına (`sessions/<id>.jsonl`) yazılır ve `exec` null olur;
oturum dışında execution ID zorunludur.

`kind`: `decision` | `unresolved` | `blocker`
`source`: `agent` | `human`

`ref` null ise → `context.unresolved_without_ref` finding'i (ADR-010).

---

## 4. Invariants

- **Ledger append-only'dir.** Hiçbir olay değiştirilmez veya silinmez; aksi
  halde geçmiş execution kanıtı sonradan yeniden yazılabilir.
- **Teşhis bilgisi kanıt değildir.** Repository raporundaki `error` ve
  `fetch_error` yalnız yönlendirme içindir, ledger'a yazılmaz; aksi halde
  geçici ağ/ortam hatası kalıcı execution gerçeği gibi görünür.
- **Finding'ler türetilir, saklanmaz.** Her `status`, ledger + Git + plan'dan
  yeniden hesaplar; aksi halde bayat finding gerçek durumla çelişir.
- **`base_sha` Runmark'un kendi ölçümüdür.** Provider'ın söylediği SHA olduğu
  gibi kaydedilmez; aksi halde ADR-002'nin evidence > claim sınırı bozulur.
- **Evidence gücü `kind`'dan türetilir.** Ayrı bir güç alanı saklanmaz; aksi
  halde aynı evidence için iki çelişen otorite oluşur.
- **Handoff ölçülen gerçekleri agent iddialarından ayrı tutar.** Runmark'un
  ürettiği bölüm ile `agent_summary` aynı başlıkta birleşmez; aksi halde iddia
  doğrulanmış veri gibi okunur.
- **Ledger'daki `base` opak bir gösterim alanıdır, ayrıştırılmaz.** Git ref'i
  her zaman güncel `project.json`'daki `remote` + `branch` alanlarından
  türetilir. Eski kayıtlar `"origin/main"` biçiminde string, yeniler aynı
  biçimde yazılmaya devam eder; ikisi de yalnızca mesajda ve handoff
  frontmatter'ında gösterilir. Aksi halde geçmiş ledger'ı ayrıştırmaya
  çalışan bir refactor, kaldırdığımız "ilk slash'tan kes" tahminini geri
  getirir ve eski kayıtları yanlış yorumlar.
- **Bloklayan ölçümü bozar, uyarı hijyeni bildirir.** ADR-014 uyarınca kanıtı
  güvenilmez yapan durumlar durur, yalnız çalışma hijyenini etkileyenler
  warning olur; aksi halde araç ya kanıtsız devam eder ya da gereksiz engeller.
- **Resume üretimdir, teslim değildir.** Ledger'a olay eklemez; state dizini
  oluşturmaz. Git fetch remote takip ref'lerini güncelleyebilir. Sonraki ajanın
  paketi okuduğunu veya talimatlara uyduğunu iddia etmez.
- **Bilinmeyen, hayır değildir.** Resume karşılaştırması veya ölçümü
  kurulamazsa ilgili alan `null`, nedeni `gaps` olur. `[]`/`false`/`0` yalnız
  gözlemin bunları desteklediği yerde kullanılır. Eksik eski provenance yeniden
  oluşturulup geçmişte kaydedilmiş gibi gösterilmez.
- **Handoff hash'i okunan byte'ları tanımlar.** Resume içerik ve SHA1'i aynı
  okumadan üretir. Üretim hash'i kayıtlıysa eşleşme ayrıca gösterilir; eksik
  hash veya uyuşmazlık gap'tir. SHA1 içerik kimliğidir, dijital imza değildir.

---

## 5. Finding

TRUST_MODEL.md'deki modelin birebir karşılığı. **Diske yazılmaz** — her
`status` çağrısında ledger + git + plan'dan yeniden hesaplanır.
Kalıcı finding store'u olsaydı cache invalidation sorunu doğardı; bunun yerine
her seferinde baştan üretmek yeterince hızlı.

```json
{
  "id": "git.stale_worktree_base",
  "severity": "warning",
  "domain": "git",
  "title": "Worktree base is stale",
  "explanation": "20260920T120000Z-SCMS-042 does not include the current origin/development",
  "evidence": [
    {"kind": "command", "ref": "git -C ~/worktrees/SCMS-042 merge-base --is-ancestor origin/development HEAD"}
  ],
  "suggested_action": "git -C ~/worktrees/SCMS-042 rebase origin/development"
}
```

`id` rastgele değil, `<domain>.<rule>` biçiminde sabit anahtar.
Bu sayede Phase 7'deki "configurable trust policies" bedava gelir —
kullanıcı kural id'siyle susturur.

`severity`: `info` | `warning` | `blocking`
`domain`: `git` | `plan` | `execution` | `context`

`git.stale_worktree_base`, açık execution'ın mevcut worktree `HEAD`'inin gözlenen
uzak ana dalı içermediğini bildirir. Rebase veya merge ile uzak ana dal alındığında
uyarı kalkar; başlangıç kaydı değiştirilmez. Karşılaştırma yapılamıyorsa
`git.worktree_base_unknown` üretilir. `status --line` son fetch edilen referansları
kullanır. Resume'daki `git.base_advanced` ise başlangıçtan beri uzak ana dalın
ilerlediğini bildiren ayrı bir tarihçe bilgisidir.

### v0.1 kural kataloğu

MVP.md §8'in birebir karşılığı. Dokuz kural, fazlası yok.

| id | severity |
|---|---|
| `git.remote_ahead` | warning |
| `git.stale_local_base` | warning |
| `git.dirty_workspace` | warning |
| `git.stale_worktree_base` | warning |
| `git.worktree_base_unknown` | warning |
| `plan.done_without_evidence` | warning |
| `plan.ambiguous_task_id` | warning |
| `plan.execution_without_plan_link` | info |
| `plan.changed_during_execution` | warning |
| `context.no_handoff` | warning |
| `context.active_execution` | info |
| `context.orphaned_execution` | warning |
| `context.invalid_ledger_timestamp` | warning |
| `context.unresolved_without_ref` | info |
| `context.hooks_not_observed` | warning |
| `context.last_test_failed` | warning |
| `context.test_result_unknown` | warning |
| `context.transcript_unavailable` | warning |
| `plan.test_claim_unverified` | warning |
| `context.unrecognised_ledger_event` | warning |
| `context.session_conflict` | warning |
| `context.unregistered_session` | warning |
| `git.orphaned_worktree` | info |

Task kimliği plan satırında **tam token** olarak aranır: pattern yalnız daha
uzun bir kimliğin parçasına uyuyorsa (`SCMS-\d+` ile `SCMS-42-W1`) satır
`plan.ambiguous_task_id` üretir ve hiçbir task'a bağlanmaz. Tahmini eşleme
kanıtı yanlış task'a bağlar; yanlış bağ, eksik bağdan kötüdür. Aynı sınır
`planReference` için de geçerlidir.

### İki sinyal kuralı

"Finish olayı yok" bir **durum değeridir**. Tek başına, şu an çalışan bir
oturumla saatler önce durmuş olanı ayıramaz — 2026-09-22'de ikisi de aynı
göründü ve iki ajan birbirinin işini tekrar yaptı.

İkinci sinyal, o execution için kaydedilmiş **en yeni olayın yaşı**dır.
`context.active_execution` ve `context.orphaned_execution` açıklamalarında,
`resume` paketinin `last_activity` alanında ve markdown çıktısında yer alır.

```text
RM-8  başladı 14:02 · son etkinlik 14:50   → biri çalışıyor
RM-5  başladı 11:18 · son etkinlik 11:34   → 4 saat sessiz
```

Ayrı bir heartbeat tutulmaz: ledger zaten her yazımı zaman damgalıyor, ve
ikinci bir kayıt anlattığı şeyle senkrondan çıkabilirdi. Kullanılabilir
zaman damgası yoksa "last activity unknown" yazılır — taze sayılmaz.

Süreç kimliği (PID) bilerek kaydedilmez: yeniden kullanılan bir PID ölü bir
execution'ı canlı gösterir ve başka makinedeki bir oturum zaten "bilinmiyor"
olurdu. Ledger'ın kendi zaman damgası daha dürüst bir sinyaldir.

`blocking` v0.1'de kullanılmıyor. Severity alanı yine de üç değerli —
ilk blocking kural geldiğinde şema değişmesin.

---

## 6. Handoff

> **Not:** Runmark'ın "handoff"u bir **belgedir** — `finish` anında yazılır ve
> bir sonraki oturum okur. CAO gibi orkestratörlerde aynı kelime **bloklayan
> bir çağrıyı** anlatır (ajanı başlat, bitmesini bekle, çıktısını al). Aynı
> kelime, iki kavram (ADR-020).

`rmk finish` üretir. Okuyucusu bir sonraki **agent**, o yüzden format
markdown; makine alanları frontmatter'da.

`.runmark/handoffs/20260918T142231Z-SCMS-042.md`

```markdown
---
exec: 20260918T142231Z-SCMS-042
task: SCMS-042
agent: codex
outcome: interrupted
repo: scms-core
worktree: ~/worktrees/SCMS-042
branch: task/SCMS-042
base: origin/development@a1b2c3d4
range: a1b2c3d4..f9e8d7c6
---

## Verified (produced by Runmark)

Commits:
- b2c3d4e  config: introduce ConfigV2 reader
- f9e8d7c  config: migrate settings loader

Files changed: 12 (+340 / -58)
- src/config/config_v2.cpp
- src/config/loader.cpp
- ...

Test: ctest 148 passed, 0 failed  (15:40)

Preserved uncommitted snapshot: refs/runmark/preserved/20260918T142231Z-SCMS-042

## Agent note (weak evidence — unverified)

Eski format okuma yolu loader.cpp'de duruyor, henüz silinmedi.

## Open items

- [ ] Eski format desteği kalacak mı?  (no ref)
- [ ] Migration testi eksik  (ref: docs/plans/config-migration.md#t9)
```

Üç başlık tesadüf değil, ADR-002'yi formatın içine gömer:
Runmark'un git'ten ölçtüğü ile agent'ın iddia ettiği aynı bölümde durmaz.

Başlıklar ve `resume --markdown` çıktısının tamamı İngilizcedir; okuyucu bir
ajan ve aynı metin iki farklı runtime'a gidiyor. Dokümanlar Türkçe kalır.

---

## 7. Handoff policy — Handoff'u kim üretir

**Melez.**

- Runmark mekanik bölümü üretir: SHA aralığı, commit'ler, değişen dosyalar,
  kaydedilmiş test sonuçları. Ölçüm, iddia değil.
- Agent'ın özeti ayrı başlıkta, `agent_summary` kind'ıyla ve
  `"unverified"` etiketiyle durur.
- Açık maddeler `note` olaylarından gelir; `ref`'i olmayan işaretlenir.

Böylece ADR-002 (evidence > agent claim) ile ADR-010 (durable source referansı)
birlikte sağlanır ve handoff hiçbir zaman saf agent çıktısı olmaz.

---

## 8. Resume paketi

`rmk resume [<task|exec>]` stdout'a JSON; `--markdown` aynı alanları
handoff.md ile aynı dilde liste ve başlıklarla sunar. Kayıtlı exec ID'ye tam
eşleşme önceliklidir; aksi halde task'ın en büyük execution ID'si seçilir
(ID içindeki UTC zamanına göre leksikografik sıralama). Execution yoksa exit 0
ve `context.no_execution` gap'i döner. Ledger okunamıyorsa yok sayılmaz:
`context.ledger_unreadable`. Geçersiz config ve komut kullanımı TC-007 hata
sözleşmesini izler.

**Seçici verilmezse projedeki en son execution seçilir.** Ledger dosya adları
UTC zaman damgasıyla başladığı için sözlük sırası kronolojidir (§1); seçim bu
sıranın en büyüğüdür.

Gerekçe: SessionStart hook'u oturum açılırken hangi task'ta olunduğunu bilmez.
Seçim mantığı çekirdekte durur — hook'un Git'ten, finding'lerden veya dosya
adlarından task çıkarması TC-006'yı bozardı. Hiç execution yoksa yine paket
döner, `context.no_execution` gap'iyle: sessiz boş çıktı ile "kayıt yok" aynı
şeye benzememeli.

Ajan hook'ları `rmk hook <session-start|prompt-submit|stop|session-end>` çağırır;
runtime'ın hook JSON'u stdin'den gelir (`session_id`, `cwd`, `transcript_path`,
`source`, `reason`, `stop_hook_active`; Claude 2.1.282 ve Codex 0.156.1'de ölçüldü).
`session-start` oturumu kaydeder ve bu paketin Markdown hâlini stdout'a yazar;
önceki bir oturum commit'ten sonra hatırlatıldığı hâlde not bırakmadan kapandıysa
"Unrecorded decisions" bölümü eklenir. `stop`, bu oturumda (kendi dizininde ya da
başlattığı execution'ların worktree'lerinde) yeni bir commit varsa ve o commit'ten
sonra bu oturumun notu yoksa commit başına bir kez `{"decision":"block","reason":…}`
döndürür; `stop_hook_active` iken asla. Ağ ve transcript okumaz; ölçülen süre 18 ms.

```json
{
  "task": "SCMS-042",
  "exec": "20260918T142231Z-SCMS-042",
  "plan_ref": "docs/plans/config-migration.md#t8",
  "plan_sha1": "<kayıtlı SHA1>",
  "current_plan_sha1": "<güncel SHA1>",
  "plan_changed": false,
  "workspace": {
    "worktree": "/worktrees/SCMS-042",
    "branch": "task/SCMS-042",
    "base": "origin/development",
    "base_sha": "a1b2c3d4",
    "remote_base_sha": "a1b2c3d4",
    "current_base_sha": "a1b2c3d4",
    "worktree_exists": true,
    "base_advanced": false
  },
  "measured": {
    "source": "execution.finished",
    "commits": ["f9e8d7c6"],
    "files_changed": 12,
    "evidence": ["<source: runtime olan evidence.recorded olayları, ledger biçiminde>"]
  },
  "agent_claims": {
    "verification": "unverified",
    "evidence": ["<ajanın yazdığı test, command ve agent_summary olayları>"]
  },
  "unresolved": {
    "with_ref": ["<ref'i dolu unresolved note olayları>"],
    "without_ref": ["<ref'i null, eksik veya boş unresolved note olayları>"]
  },
  "instructions": [{"name": "AGENTS.md", "path": "/projects/scms/AGENTS.md", "sha1": "<SHA1>"}],
  "preserved_ref": null,
  "handoff": {
    "path": "/projects/scms/.runmark/handoffs/20260918T142231Z-SCMS-042.md",
    "sha1": "<okunan byte'ların SHA1'i>",
    "recorded_sha1": "<finish'te kaydedilen SHA1>",
    "verified": true,
    "content": "<handoff dosyasının UTF-8 içeriği>"
  },
  "gaps": []
}
```

Dizilerdeki `<... olayları>` yer tutucuları gerçek çıktıda JSON nesneleridir.
Bitmiş execution ölçümleri finish olayının zamanına aittir; worktree silinse de
korunur. Aktif execution için `source: "git"`, ayrıca `head_sha` ve o anda
ölçülen `base_sha..HEAD` commit/dosya sayısı gelir. Commit edilmemiş dosyalar
bu sayıya dahil değildir; varsa `preserved_ref` snapshot'ı ayrıca gösterilir.
Kaydedilmiş test/command evidence'ı yeniden çalıştırılmaz; agent iddiasıyla
aynı alana konmaz. Talimatların güncel içeriği eski hash'in yerine yazılmaz.

`plan_changed` ve `base_advanced`: `true | false | null`. Base için başarılı
fetch gerekir; SHA değişmiş ve eski uç yeninin atasıysa `true`, aynıysa `false`;
fetch/ancestry hatası, değişen base config'i veya ayrışan geçmişte `null` + gap.
`handoff.verified`: `true | false | null`; hash uyuşmazlığında `false`.
`worktree_exists` gözlem kurulamadığında `null` olur.
Okunamayan handoff'un `sha1` ve `content` alanları `null`, yolu biliniyorsa doludur.
Execution yoksa `exec`, plan/workspace değerleri, ölçümün `source`/`commits`/
`files_changed` alanları, `instructions`, `preserved_ref` ve handoff değerleri
`null`; seçilmiş olaya ait evidence/note dizileri boştur ve neden gap'tedir.
Eski kayıtlarda eksik `instructions`: `null` + gap; açıkça kayıtlı `[]`: talimat yok.

`gaps` her zaman finding şemasında `id/severity/domain/title/explanation` taşır;
değerlendirme `rules.cpp::evaluateResume(ResumeFacts)` içinde saf yapılır.

| Durum | Gap ID |
|---|---|
| Execution yok / ledger okunamıyor / start eksik | `context.no_execution` / `context.ledger_unreadable` / `context.missing_start` |
| Handoff yok / okunamıyor | `context.no_handoff` / `context.handoff_unreadable` |
| Üretim hash'i yok / dosya değişmiş | `context.handoff_unverified` / `context.handoff_changed` |
| Worktree yok / varlığı bilinmiyor | `git.worktree_missing` / `git.worktree_unknown` |
| Base ilerlemiş / karşılaştırma bilinmiyor / fetch hatası | `git.base_advanced` / `git.base_unknown` / `git.fetch_failed` |
| Plan değişmiş / karşılaştırma bilinmiyor / plan linki yok | `plan.changed_during_execution` / `plan.comparison_unknown` / `plan.execution_without_plan_link` |
| Commit/dosya ölçümü kurulamadı | `git.measurement_unavailable` |
| Talimat kaydı yok / talimat hash'i yok | `context.instructions_unknown` / `context.instruction_unreadable` |
| Geçersiz başlangıç zamanı / refsiz açık madde | `context.invalid_ledger_timestamp` / `context.unresolved_without_ref` |

## 9. v0.1'de bilerek yok

| Yok | Ne zaman eklenir |
|---|---|
| `cache/`, `index/` | `status` 200 ms'yi geçince |
| Finding persistence | Geçmiş finding trendi istenirse |
| Multi-repo çapraz execution | `repos` ikinci elemanı geldiğinde |
| Provider interface | İkinci plan provider yazılırken |
| Testi Runmark'ın kendisinin koşması | Runtime kaydı da yetmeyince (ADR-021) |
| Ledger şema versiyonu | İlk breaking change'de (`project.json` `version` var) |
