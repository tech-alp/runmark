#include "FindingModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace runmark {
namespace {
struct Copy { QString title; QString summary; QString impact; QString next; };
Copy present(const FindingModel::Row& row)
{
    const auto tr = [](const char* text) { return FindingModel::tr(text); };
    const QString id = row.id;
    const auto match = QRegularExpression(QStringLiteral("^\\d{8}T\\d{6}Z-(\\S+)")).match(row.explanation);
    const QString task = match.hasMatch() ? match.captured(1) : QString();
    if (id == "context.unresolved_without_ref")
        return {tr("%1 notta kaynak bağlantısı eksik").arg(row.notes.size()),
            tr("Notlar kayıtlı; dayandıkları belge veya kanıt belirtilmemiş."),
            tr("Bu notların çözüldüğü ya da geçerliliğini yitirdiği sonucuna varılamaz."),
            tr("Aşağıdaki notları inceleyin. İlgili çalışma için kaynak referansı içeren bir takip notu kaydedin.")};
    if (id == "plan.execution_without_plan_link")
        return {task.isEmpty() ? tr("Çalışmanın plan bağlantısı kaydedilmemiş") : tr("%1: plan bağlantısı kaydedilmemiş").arg(task),
            tr("Başlangıç kaydında plan maddesine bağlantı bulunmuyor."),
            tr("Bu, görevin bugün planda olmadığı anlamına gelmez. Başlangıç kaydı ile plan arasındaki ilişki doğrulanamıyor."),
            tr("Güncel planda görev kodunu arayın; başlangıç kaydını inceleyip eşleşmeyi kaynaklı bir takip notuyla açıklayın.")};
    if (id == "git.dirty_workspace")
        return {tr("Commit edilmemiş değişiklikler var"), tr("%1: kaydedilmeyi bekleyen dosya değişiklikleri var.").arg(row.explanation.section(" is dirty", 0, 0)),
            tr("Çalışma klasörünün içeriği son commit ile aynı değil. Bu tek başına hata değildir."),
            tr("Git diff ile değişiklikleri inceleyin; hazır olanları commit edin. Değişiklikleri otomatik silmeyin.")};
    static const QHash<QString, Copy> copies = {
        {"git.fetch_failed", {tr("Uzak depoya erişilemedi"), tr("Uzak deponun güncel durumu alınamadı."), tr("Uzak dalla karşılaştırma eksik veya eski olabilir."), tr("Ağ erişimini ve Git kimlik bilgilerini kontrol edip Yenile'ye basın.")}},
        {"git.remote_ahead", {tr("Çalışma dalı uzak ana dalın gerisinde"), tr("Uzak ana dalda bu çalışma dalına alınmamış commitler var."), tr("Çalışmanız güncel değişikliklerle henüz karşılaştırılmamış olabilir."), tr("Git geçmişini ve farkları inceleyip uygun birleştirme yöntemini seçin.")}},
        {"git.stale_local_base", {tr("Yerel ana dal güncel değil"), tr("Yerel ana dal uzak ana dalın gerisinde."), tr("Yerel ana dalın gösterdiği durum uzak depoyu yansıtmıyor."), tr("Yerel değişiklikleri kontrol ederek ana dalı güncelleyin.")}},
        {"git.stale_worktree_base", {tr("Çalışma alanı uzak ana dalın gerisinde"), tr("Çalışma dalı uzak ana dalın güncel commitini içermiyor."), tr("Devam etmeden önce yeni değişikliklerin etkisini incelemek gerekiyor."), tr("Uzak ana dalla farkları inceleyin; gerekirse çalışma dalını güncelleyin.")}},
        {"git.worktree_base_unknown", {tr("Çalışma alanının güncelliği ölçülemedi"), tr("Çalışma alanı uzak ana dalla karşılaştırılamadı."), tr("Çalışma dalının güncel olduğu doğrulanamıyor."), tr("Çalışma klasörünü ve Git erişimini kontrol edip Yenile'ye basın.")}},
        {"git.orphaned_worktree", {tr("Tamamlanan çalışmanın klasörü duruyor"), tr("Bitiş kaydı var; çalışma klasörü hâlâ diskte."), tr("Bu klasörün silinmeye uygun olduğu henüz doğrulanmış değil."), tr("Commit edilmemiş veya birleştirilmemiş değişiklikleri kontrol etmeden klasörü silmeyin.")}},
        {"context.no_handoff", {tr("Devir özeti bulunamadı"), tr("Tamamlanan çalışma için devir dosyası bulunamadı."), tr("Sonraki oturum çalışma sonucunu eksik devralabilir."), tr("Çalışma kaydını ve devir dosyasının konumunu kontrol edin.")}},
        {"context.invalid_ledger_timestamp", {tr("Çalışma kaydının tarihi geçersiz"), tr("Başlangıç tarihi okunamıyor."), tr("Çalışmanın ne kadar süredir açık olduğu hesaplanamıyor."), tr("Teknik kayıttaki tarih değerini ve kaydı üreten aracı inceleyin.")}},
        {"context.orphaned_execution", {tr("Uzun süredir kapanmamış çalışma var"), tr("En az 24 saattir bitiş veya devir kaydı bulunmuyor."), tr("Çalışmanın gerçekten sürüp sürmediği doğrulanamıyor."), tr("İlgili oturumu kontrol edin; sonucu doğruladıktan sonra çalışmayı kapatın veya devam edin.")}},
        {"context.active_execution", {tr("Çalışmanın kapanış kaydı yok"), tr("Henüz bitiş veya devir kaydı bulunmuyor."), tr("Bu kayıt, ajanın şu anda çalıştığını tek başına kanıtlamaz."), tr("İlgili oturumun durumunu kontrol edin; iş bittiyse sonucunu kaydedin.")}},
        {"plan.changed_during_execution", {tr("Çalışma sırasında plan değişmiş"), tr("Güncel plan başlangıçta kaydedilen planla aynı değil."), tr("Görevin kapsamı veya kabul koşulları değişmiş olabilir."), tr("Plan farklarını inceleyip devam eden görevle uyumunu kontrol edin.")}},
        {"plan.unreadable", {tr("Plan dosyası okunamıyor"), tr("Yapılandırılan plan dosyasına erişilemedi."), tr("Plan kontrolleri çalıştırılamadı; sonuçlar eksik."), tr("Proje yapılandırmasındaki plan yolunu ve dosya izinlerini kontrol edin.")}},
        {"plan.done_without_evidence", {tr("Tamamlanan görev için kanıt yok"), tr("Görev tamamlandı işaretli, ancak ilişkili çalışma kanıtı bulunamadı."), tr("Tamamlanma durumu kayıtlı kanıtla doğrulanamıyor."), tr("İlgili görev için test veya değişiklik kanıtını kaydedin; tamamlandı işaretini gözden geçirin.")}},
        {"plan.ambiguous_task_id", {tr("Plan satırındaki görev kodu belirsiz"), tr("Görev deseni daha uzun bir kodun yalnızca bir bölümünü eşleştiriyor."), tr("Kanıt yanlış göreve bağlanabileceği için eşleştirme yapılmadı."), tr("Görev kodunu veya yapılandırmadaki görev desenini düzeltin.")}},
        {"plan.no_parsable_tasks", {tr("Planda görev tanınamadı"), tr("Plan okunabildi, ancak uygun görev maddesi bulunamadı."), tr("Hiçbir plan görevi değerlendirilemedi."), tr("Planı '- [ ] PROJ-1 Açıklama' biçiminde yazın; görev önekinin yapılandırmayla eşleştiğini kontrol edin.")}},
        {"context.hooks_not_observed", {tr("Ajan bağlantısı henüz gözlenmedi"), tr("Ajan eklentisi bekleniyor, fakat hook kaydı bulunmuyor veya okunamıyor."), tr("Oturum devamlılığının otomatik sağlandığı doğrulanamıyor."), tr("Runmark ajan eklentisini ve hook kaydını kontrol edip yeni bir oturum açın.")}}
    };
    Copy copy = copies.value(id, {tr("Yeni bir bulgu kaydedildi"), tr("Bu bulgu için Türkçe açıklama henüz tanımlı değil."), tr("Asıl başlık ve açıklama teknik kayıtta korunuyor."), tr("Teknik kaydı inceleyin.")});
    if (!task.isEmpty()) copy.title = task + ": " + copy.title;
    if (id == "plan.done_without_evidence" || id == "plan.changed_during_execution") copy.title = row.explanation + ": " + copy.title;
    return copy;
}
}


int FindingModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant FindingModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row& row = m_rows.at(index.row());
    const auto copy = present(row);
    const bool urgent = row.severity != "info";
    switch (role) {
    // Rule IDs repeat across repositories/tasks. Preserve selection only for
    // the same finding content, never for another instance of the same rule.
    case KeyRole: return QString::fromUtf8(QJsonDocument(QJsonArray{
        row.id, row.domain, row.title, row.explanation, row.suggestedAction}).toJson(QJsonDocument::Compact));
    case DisplayTitleRole: return copy.title;
    case SummaryRole: return copy.summary;
    case ImpactRole: return copy.impact;
    case NextStepRole: return copy.next;
    case NotesRole: return row.notes;
    case OccurrencesRole: return row.notes.isEmpty() ? 1 : row.notes.size();
    case PriorityRole: return row.severity == "critical" || row.severity == "error" ? 0 : row.severity == "warning" ? 1 : 2;
    case SectionRole: return urgent ? tr("İlgilenmeniz gerekenler") : tr("Bilgi kayıtları");
    case SourceUrlRole: return row.domain == "plan" ? m_plan : row.domain == "git" ? m_projectFolder : QUrl();
    case SourceLabelRole: return row.domain == "plan" ? tr("Güncel planı aç") : tr("Proje klasörünü aç");
    case IdRole: return row.id;
    case SeverityRole: return row.severity;
    case DomainLabelRole: return domainLabel(row.domain);
    case DomainRole: return row.domain;
    case TitleRole: return row.title;
    case ExplanationRole: return row.explanation;
    case SuggestedActionRole: return row.suggestedAction;
    default: return {};
    }
}

QHash<int, QByteArray> FindingModel::roleNames() const
{
    return {{IdRole, "findingId"}, {SeverityRole, "severity"}, {DomainRole, "domain"},
        {DisplayTitleRole, "displayTitle"}, {SummaryRole, "summary"}, {ImpactRole, "impact"},
        {NextStepRole, "nextStep"}, {NotesRole, "notes"}, {OccurrencesRole, "occurrences"},
        {PriorityRole, "priority"}, {SectionRole, "sectionLabel"}, {SourceUrlRole, "sourceUrl"}, {SourceLabelRole, "sourceLabel"},
        {TitleRole, "title"}, {ExplanationRole, "explanation"}, {SuggestedActionRole, "suggestedAction"}, {KeyRole, "findingKey"}, {DomainLabelRole, "domainLabel"}};
}

QString FindingModel::domainLabel(const QString& domain)
{
    if (domain == QLatin1String("git")) return tr("Git");
    if (domain == QLatin1String("plan")) return tr("Plan");
    if (domain == QLatin1String("context")) return tr("Bağlam");
    if (domain == QLatin1String("documentation")) return tr("Dokümantasyon");
    return domain;
}

void FindingModel::setSourceLocations(const QUrl& projectFolder, const QUrl& plan)
{
    m_projectFolder = projectFolder;
    m_plan = plan;
    if (!m_rows.isEmpty()) emit dataChanged(index(0), index(m_rows.size() - 1), {SourceUrlRole, SourceLabelRole});
}

void FindingModel::reset(const QVector<Row>& rows)
{
    beginResetModel();
    m_rows.clear();
    int notesIndex = -1;
    for (const auto& row : rows) {
        if (row.id != "context.unresolved_without_ref") { m_rows.append(row); continue; }
        if (notesIndex < 0) {
            notesIndex = m_rows.size();
            m_rows.append(row);
            m_rows.last().notes.clear();
            m_rows.last().explanation.clear();
        }
        m_rows[notesIndex].notes.append(row.explanation);
        m_rows[notesIndex].explanation = m_rows[notesIndex].notes.join("\n\n");
    }
    endResetModel();
}

} // namespace runmark
