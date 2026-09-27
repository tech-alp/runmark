# RM-15 — Nimbalyst incelemesi (Faz 0b öncesi)

Tarih: 2026-09-26. Karar sahibi: kullanıcı. Bu rapor [Runmark kokpit tasarımındaki](runmark-cockpit.md) Faz 0b kararına girdi sağlar.

## Ölçüm sınırı

- Yerelde kurulu Nimbalyst **0.77.5** açıldı. Geçici `/private/tmp/rm15-nimbalyst-fixture` projesine `task_plan.md`, `docs/plans/migration.md` ve `plans/sample.md` kondu. Her dosyada bir `- [x]`, bir `- [ ]` satırı vardı. AI oturumu açılmadı; MCP sunucusu bağlanmadı.
- Resmi [README](https://github.com/Nimbalyst/nimbalyst/blob/main/README.md), [Quickstart](https://docs.nimbalyst.com/getting-started/quickstart), [MCP dokümanı](https://docs.nimbalyst.com/setup-nimbalyst/mcp), [extension mimarisi](https://github.com/Nimbalyst/nimbalyst/blob/main/docs/EXTENSION_ARCHITECTURE.md) ve [0.78.5 sürüm notu](https://github.com/Nimbalyst/nimbalyst/releases/tag/v0.78.5) incelendi. **0.78.5 kurulup denenmedi**; yerel sonuçlar yalnız 0.77.5 içindir.
- Geçici projenin dışında Runmark planlarının gerçek Nimbalyst projesinde taranması, canlı Claude/Codex oturumu takibi, MCP ile Runmark bağlantısı, extension SDK ile panel yapımı ve Nimbalyst API'sine veri yazma **denenmedi**.

## 1. İş akışı dosyalarını okuyor mu?

**Dosya olarak evet; otomatik plan ilerlemesi olarak gözlenmedi.** Yerel denemede `task_plan.md` dosyası Files görünümünde açıldı; `- [ ] Build parser` işaretsiz, `- [x] Draft design` işaretli checkbox olarak göründü. İç içe `docs/plans/migration.md` de dosya ağacında erişilebilirdi. Bu, [Quickstart'ın](https://docs.nimbalyst.com/getting-started/quickstart) standart Markdown dosyalarını açma vaadiyle uyumlu.

Aynı projede Tracker görünümü **`0 open of 0`**, **`Plans 0`**, **`Tasks 0`** gösterdi. `Import > Import from plans/` komutu, `plans/sample.md` içindeki sade checkbox listesi için **`No items found`** döndürdü. `docs/plans/` için doğrudan içe aktarma seçeneği görülmedi. Bu ölçüm, sade `- [ ]` satırlarının Tracker kartına veya dosya başına N/M sayısına kendiliğinden dönüşmediğini gösterir; bütün olası içe aktarma biçimlerini dışlamaz.

Nimbalyst'in kendi [eğitimi](https://github.com/nimbalyst/skills/blob/main/skills/getting-started/tutorial.md), Markdown planını belge olarak açıp ayrıca agent ile tracker öğelerine dönüştürmeyi tarif ediyor. Bu adım otomatik, kaynak dosyaya bağlı, deterministik ilerleme okuyucusu yerine geçmez. Nimbalyst'in [tracker öğeleri](https://github.com/Nimbalyst/nimbalyst/blob/main/docs/THE_HARNESS.md) plan, karar ve görev türlerini destekler; Runmark'ın çeşitli araçların mevcut checklist dosyalarını aynen sayma şartı başka bir iştir.

## 2. Dışarıdan kaynak bağlanabiliyor mu?

| Yol | Kanıt | Runmark açısından sınır |
|---|---|---|
| Proje dosyaları | Yerel fixture açıldı; [Quickstart](https://docs.nimbalyst.com/getting-started/quickstart) proje klasöründeki standart dosyaları tarif ediyor. | Runmark Markdown dosyası açılabilir. Dosyadaki checkbox'ların Tracker veya kokpit verisine otomatik alınması ölçülmedi; sade listede alınmadı. |
| MCP istemcisi | [Resmi MCP dokümanı](https://docs.nimbalyst.com/setup-nimbalyst/mcp) uygulama/proje düzeyinde özel sunucu eklemeyi ve araçların ajana sunulmasını açıklıyor. | Runmark MCP sunucusu geliştirilirse **ajan** sorgulayabilir. MCP sonucunun Nimbalyst'in yerleşik Tracker/Kanban ekranına veri kaynağı olması belgelenmedi ve denenmedi. |
| Extension SDK | [Resmi mimari](https://github.com/Nimbalyst/nimbalyst/blob/main/docs/EXTENSION_ARCHITECTURE.md) özel editör, panel, widget ve MCP araç katkılarını tarif ediyor. | Ayrı Runmark paneli teknik olarak mümkün görünüyor; SDK üzerinden Runmark verisini okuyup panelde gösteren uçtan uca örnek **denenmedi**. Bu, kendi Qt kokpitine karşı bakım maliyeti taşıyan bir entegrasyondur. |
| Genel uygulama API'si | İncelenen resmi kaynaklarda Runmark verisini Tracker'a besleyecek kararlı, dışarıya açık API sözleşmesi doğrulanmadı. | **Denenmedi / doğrulanmadı.** Var olmadığı iddia edilmiyor; üzerine Faz 0b planı kurulmamalı. |

### MCP burada ne demek?

**MCP (Model Context Protocol), Nimbalyst içindeki ajana araç bağlama yolu.** [Nimbalyst dokümanına](https://docs.nimbalyst.com/setup-nimbalyst/mcp) göre kullanıcı bir sunucuyu uygulama veya proje düzeyinde kaydeder; araçları Claude/Codex oturumu çağırır. Runmark bugün MCP sunucusu sunmuyor ([kokpit tasarımındaki mevcut durum](runmark-cockpit.md)). İleride gerekirse küçük bir sunucu `rmk status` ve `rmk resume` sonuçlarını `runmark_status` / `runmark_resume` gibi araçlarla ajana verebilir. Bu adlar **örnek taslak**, mevcut API değil; sunucu ve bağlantı **denenmedi**.

Örnek akış: Nimbalyst'te kullanıcı “Bu işte nerede kaldık?” diye sorar → ajan Runmark MCP aracını çağırır → yanıt sohbette görünür. Bu, Nimbalyst Tracker kartlarını, Kanban'ı veya bildirimleri kendiliğinden Runmark verisiyle doldurmaz. [Resmi doküman](https://docs.nimbalyst.com/setup-nimbalyst/mcp) MCP'yi ajanın dış servislere erişimi olarak tarif ediyor; panel veri kaynağı sözleşmesi olarak değil. Yalnız kokpit ekranı için MCP yazmak bu nedenle gereksiz ek iş olur.

### “Panel prototipi” tam olarak ne?

**Önerilen, henüz yapılmamış deneme:** Nimbalyst içine tek projelik, salt okunur bir Runmark sekmesi eklemek. [Extension Panels dokümanı](https://github.com/Nimbalyst/nimbalyst/blob/main/docs/EXTENSION_PANELS.md) extension manifestinde `placement: "fullscreen"` panelini ve `PanelHost.workspacePath` / `openFile()` bağlarını tarif ediyor. Bu doküman upstream `main` için; kurulu 0.77.5'te extension yükleme ve panel davranışı **denenmedi**.

Panelin ilk sürümü yalnız mevcut Runmark verisini gösterir: dosya başına tamamlanan/toplam plan maddesi, aktif/bekleyen oturumlar, son not veya handoff ve bir gerçek bulgu (örneğin çakışma). Kaynak adayları bugün ölçülen `rmk status` JSON çıktısındaki `plans`, `sessions`, `findings` alanları ve seçilen iş için `rmk resume <exec>` çıktısıdır. **Kritik ilk adım:** Nimbalyst extension'ının proje yolunda Runmark CLI çıktısına desteklenen bir yöntemle erişip erişemediğini doğrulamak. Panel dokümanındaki `PanelHost` arayüzü bir CLI çalıştırma metodu göstermiyor; doğrudan süreç başlatabildiği varsayılmamalı. Gerekirse ayrı bir yerel, salt okunur veri köprüsünün maliyeti ölçülür; prototipten önce kalıcı servis veya yeni Runmark API'si tasarlanmaz.

**Geçme ölçütü:** Bir gerçek projede panelin N/M sayıları `rmk status` ile birebir tutar; oturum durumu ve bulgu güncellemeden sonra yenilenir; plan dosyasına gidilebilir; plan, tracker ve Runmark kayıtları panel tarafından değiştirilmez. CLI verisine güvenli erişim için gereken kod ve bakım yükü de kaydedilir. Bunlar sağlanmadan “Nimbalyst Runmark kokpitidir” denmez. Prototip çok projeli liste, görev düzenleme, ajan başlatma veya yeni bildirim üretmez; bunlar RM-16 yön kararı sonrasının kapsamıdır.

## 3. Runmark kokpit ihtiyaçlarıyla örtüşme

| [Kokpit ihtiyacı](runmark-cockpit.md) | Nimbalyst bulgusu | Değerlendirme |
|---|---|---|
| Paralel Claude/Codex oturumları, iş ağaçları, Kanban | [README](https://github.com/Nimbalyst/nimbalyst/blob/main/README.md) ve [Quickstart](https://docs.nimbalyst.com/getting-started/quickstart) bunları tarif ediyor; yerel uygulamada Agent/Tracker/Files sekmeleri görüldü. | **Güçlü örtüşme**; canlı Runmark oturumuyla denenmedi. |
| Terminalde Nimbalyst dışında açılan oturumlar | [0.78.5 sürüm notu](https://github.com/Nimbalyst/nimbalyst/releases/tag/v0.78.5) harici Claude/Codex CLI oturumlarını canlı izlemeyi **alpha** olarak duyuruyor. | **Kısmi**; kurulu 0.77.5'te ve Runmark hook oturumlarıyla denenmedi. |
| “Ajan senden yanıt bekliyor” bildirimi ve kurtarma | [Quickstart](https://docs.nimbalyst.com/getting-started/quickstart) mobil dashboard, bekleyen onay bildirimi ve devam ettirmeyi; [bildirim dokümanı](https://docs.nimbalyst.com/setup-nimbalyst/ai-provider-setup-and-notifications) masaüstü yanıt bildirimini anlatıyor. | **Kısmi/güçlü**; gerçek bildirim ve Runmark'ın kayıp worktree/eski base kurtarması denenmedi. |
| Çok projeli tek görünüm | [Quickstart](https://docs.nimbalyst.com/getting-started/quickstart) birden fazla proje penceresini destekliyor. | **Kısmi**; tek birleşik N/M + oturum + not panosu doğrulanmadı. |
| Farklı araçların `- [ ]` planlarından dosya başına N/M | Yerel denemede checkbox görüntülendi; Tracker `0 open of 0` kaldı ve sade `plans/` içe aktarımı `No items found` verdi. | **Boşluk**. |
| `rmk note`, handoff, doğrulanmış kanıt ve çakışma radarı | Nimbalyst [tracker kararlarını](https://github.com/Nimbalyst/nimbalyst/blob/main/docs/THE_HARNESS.md) ve oturum bağlantılarını destekliyor; [README](https://github.com/Nimbalyst/nimbalyst/blob/main/README.md) diff/commit incelemesini anlatıyor. | **Runmark semantiği ölçülmedi**: notsuz kapanış, eski base, dosya/görev çakışması ve ölçülen kanıtın aynı kurallarla gösterimi için bağ gerekir. |
| Planlara salt okunur bakış, tek yazar | [Quickstart](https://docs.nimbalyst.com/getting-started/quickstart) dosyaların uygulama içinde düzenlenip otomatik kaydedildiğini anlatıyor; yerel Files görünümü de editör açtı. | **Tasarım gerilimi**: Runmark paneli salt okunur olmalı; Nimbalyst'in normal editöründe aynı dosyayı elle düzenlemek iki yazar riskini taşır. |

## Öneri

**Runmark kendi süreklilik motoru olarak kalsın; Nimbalyst'i Faz 0b için tercih edilen görüntüleyici adayı yapın.** Oturum/Kanban/bildirim/worktree arayüzünü tekrar yazmanın getirisi düşük. Runmark'ın farkı, mevcut plan dosyalarını deterministik sayması, oturum devri, not ve çakışma kanıtını üretmesi. Tracker'a kopya görev yazıp ikinci bir kaynak oluşturmak yerine önce **salt okunur, tek projelik Runmark paneli** için Nimbalyst extension SDK ile dar bir prototip ölçülmeli. MCP yalnız ajan erişimi sağladığı için pano entegrasyonunun kanıtı sayılmamalı. Prototip mevcut N/M, aktif oturum, son not ve bir çakışma uyarısını dosyaları değiştirmeden gösterirse RM-16 Nimbalyst entegrasyonuna döner; bu mümkün değilse Runmark'ın ince Qt kokpiti yapılır. Bu bir öneridir, yön kararı kullanıcıdadır.

**Açık karar:** RM-16'nın hedefi Nimbalyst paneli mi, Runmark'ın kendi ince çok projeli Qt kokpiti mi? Dar panel prototipinin maliyeti ve harici oturumların gerçek yakalanması görülmeden kesinleştirilmedi.

## İnceleme (2026-09-27)

Ölçüm kısmı sağlam; öneri kanıtın önüne geçiyor. Önerilen panel prototipi RM-16'yı belirleyecek soruları test etmiyor.

1. **Önceden yazılan ölçüt değişti.** [Kokpit tasarımı](runmark-cockpit.md) "iş akışı dosyalarını okumuyor ve dışarıdan kaynak bağlanabiliyorsa (MCP ya da dosya) Runmark motor olur" diyordu. Bu rapora göre MCP yalnız ajanı besliyor, ekranı beslemiyor; ikinci şart bugün karşılanmıyor. Ölçüte göre varsayılan sonuç kendi kokpitimiz. Extension paneli yeni bir ölçüt; Nimbalyst'i "tercih edilen aday" yapmak için henüz kanıt yok.
2. **Prototip geçse de 0b kanıtlanmaz.** 0b bitti ölçütü çok projeli görünüm, kurtarma düğmeleri ve "ajan bekliyor" bildirimi istiyor. Prototip tek projelik, salt okunur ve düğmesiz; zor kısımlar kapsam dışında.
3. **"Yeniden yazmayalım" gerekçesi denenmemiş özelliğe dayanıyor.** Nimbalyst'in oturum, Kanban ve bildirimleri kendi içinde açılan oturumlar için. Kullanıcı ajanları terminalde (cmux) çalıştırıyor; harici oturum izleme 0.78.5'te alpha ve denenmedi. Kokpit tasarımı bu özellikleri zaten "emtia" sayıyor.
4. **İki kısıt atlandı.** ADR-022 gereği Runmark üçüncü taraf araç kurulu olmadan çalışır; Nimbalyst en fazla isteğe bağlı görüntüleyici olabilir, Runmark'ın kendi görünümü her durumda gerekir. Karşılaştırma Qt kokpitini sıfırdan sayıyor; oysa `apps/desktop` ve `libs/ui-shell` findings görünümü var ve `rmk status` bugün `plans`, `sessions`, `findings` alanlarını veriyor.
5. **Veri köprüsü daha basit olabilir.** [Extension mimarisi](https://github.com/Nimbalyst/nimbalyst/blob/main/docs/EXTENSION_ARCHITECTURE.md) masaüstünde `permissions.filesystem` ile dosya okumayı tarif ediyor; süreç başlatma yok. En ucuz köprü: Runmark'ın her olayda çalışan hook'larla `.runmark/` altına yazdığı bir durum dosyasını panelin okuması; kurallar C++'ta kalır. Upstream `main` belgesi; 0.77.5'te doğrulanmadı. Belgede API kararlılığı taahhüdü yok; tek kullanıcılı araç için bakım riski.

Raporun doğru tespitleri: MCP'nin pano entegrasyonu için gereksiz olduğu, iki yazar riski ve "denenmedi" işaretleri.

### RM-16 için eksik kanıt (öncelik sırasıyla)

1. **Kullanım:** Kullanıcı Nimbalyst'i gün boyu cmux yerine ya da yanında açık tutar mı? Hayırsa diğerleri önemsiz.
2. **Harici oturum:** 0.78.5, runmark projesinde terminalden açılan gerçek bir Claude ve bir Codex oturumunu izliyor mu? (~30 dk)
3. **Çok proje:** Tek pencerede birden fazla proje görünür mü? (0b'nin ilk ölçütü)
4. **Yazma eylemi:** Panel bir komut tetikleyebilir mi? Tetikleyemiyorsa kurtarma düğmeleri Nimbalyst'te yapılamaz.
5. **Extension denemesi:** Kurulu sürümde bir JSON dosyasını okuyup gösteren en küçük panel. (1–2 saat)
6. **Karşılaştırma tabanı:** Mevcut Qt desktop bugün neyi gösteriyor, 0b ölçütüne ne kadar iş kalıyor?

**Yol:** Önce 1 ve 2 (ucuz). Biri "hayır" ise RM-16 mevcut Qt desktop üzerinde ince kokpit olur; Nimbalyst sonraya isteğe bağlı görüntüleyici olarak kalır. İkisi de "evet" ise 3–5 denenir.

### Karar (2026-09-27)

Kanıt 1'in cevabı **hayır**: kullanıcı Nimbalyst'i aktif kullanmıyor; paralel ajanları macOS'ta cmux, Linux'ta herdr ile yönetiyor. RM-16 mevcut Qt desktop üzerinde ince kokpit olarak yapılır; Nimbalyst sonraya isteğe bağlı görüntüleyici olarak kalır. Kanıt 2–5 gereksizleşti.
