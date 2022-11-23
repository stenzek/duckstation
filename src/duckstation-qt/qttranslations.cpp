#include "common/log.h"
#include "common/string_util.h"
#include "fmt/format.h"
#include "frontend-common/imgui_manager.h"
#include "imgui.h"
#include "qthost.h"
#include <QtCore/QFile>
#include <QtCore/QTranslator>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMessageBox>
#include <vector>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

Log_SetChannel(QTTranslations);

namespace QtHost {
static const ImWchar* GetGlyphRangesJapanese();
static const ImWchar* GetGlyphRangesChinese();
static std::string GetFontPath(const char* name);
} // namespace QtHost

static std::vector<QTranslator*> s_translators;

void QtHost::InstallTranslator()
{
  for (QTranslator* translator : s_translators)
  {
    qApp->removeTranslator(translator);
    translator->deleteLater();
  }
  s_translators.clear();

  const QString language(QString::fromStdString(Host::GetBaseStringSettingValue("Main", "Language", "en")));

  // install the base qt translation first
  const QString base_dir(QStringLiteral("%1/translations").arg(qApp->applicationDirPath()));
  QString base_path(QStringLiteral("%1/qtbase_%2.qm").arg(base_dir).arg(language));
  bool has_base_ts = QFile::exists(base_path);
  if (!has_base_ts)
  {
    // Try without the country suffix.
    const int index = language.indexOf('-');
    if (index > 0)
    {
      base_path = QStringLiteral("%1/qtbase_%2.qm").arg(base_dir).arg(language.left(index));
      has_base_ts = QFile::exists(base_path);
    }
  }
  if (has_base_ts)
  {
    QTranslator* base_translator = new QTranslator(qApp);
    if (!base_translator->load(base_path))
    {
      QMessageBox::warning(
        nullptr, QStringLiteral("Translation Error"),
        QStringLiteral("Failed to find load base translation file for '%1':\n%2").arg(language).arg(base_path));
      delete base_translator;
    }
    else
    {
      s_translators.push_back(base_translator);
      qApp->installTranslator(base_translator);
    }
  }

  const QString path = QStringLiteral("%1/duckstation-qt_%3.qm").arg(base_dir).arg(language);
  if (!QFile::exists(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(language).arg(path));
    return;
  }

  QTranslator* translator = new QTranslator(qApp);
  if (!translator->load(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(language).arg(path));
    delete translator;
    return;
  }

  Log_InfoPrintf("Loaded translation file for language %s", language.toUtf8().constData());
  qApp->installTranslator(translator);
  s_translators.push_back(translator);

#ifdef _WIN32
  if (language == QStringLiteral("ja"))
  {
    ImGuiManager::SetFontPath(GetFontPath("msgothic.ttc"));
    ImGuiManager::SetFontRange(GetGlyphRangesJapanese());
  }
  else if (language == QStringLiteral("zh-cn"))
  {
    ImGuiManager::SetFontPath(GetFontPath("msyh.ttc"));
    ImGuiManager::SetFontRange(GetGlyphRangesChinese());
  }
#endif
}

static std::string QtHost::GetFontPath(const char* name)
{
#ifdef _WIN32
  PWSTR folder_path;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &folder_path)))
    return fmt::format("C:\\Windows\\Fonts\\%s", name);

  std::string font_path(StringUtil::WideStringToUTF8String(folder_path));
  CoTaskMemFree(folder_path);
  font_path += "\\";
  font_path += name;
  return font_path;
#else
  return name;
#endif
}

std::vector<std::pair<QString, QString>> QtHost::GetAvailableLanguageList()
{
  return {{QStringLiteral("English"), QStringLiteral("en")},
          {QStringLiteral("Deutsch"), QStringLiteral("de")},
          {QStringLiteral("Español de Hispanoamérica"), QStringLiteral("es")},
          {QStringLiteral("Español de España"), QStringLiteral("es-es")},
          {QStringLiteral("Français"), QStringLiteral("fr")},
          {QStringLiteral("עברית"), QStringLiteral("he")},
          {QStringLiteral("日本語"), QStringLiteral("ja")},
          {QStringLiteral("Italiano"), QStringLiteral("it")},
          {QStringLiteral("Nederlands"), QStringLiteral("nl")},
          {QStringLiteral("Polski"), QStringLiteral("pl")},
          {QStringLiteral("Português (Pt)"), QStringLiteral("pt-pt")},
          {QStringLiteral("Português (Br)"), QStringLiteral("pt-br")},
          {QStringLiteral("Русский"), QStringLiteral("ru")},
          {QStringLiteral("Türkçe"), QStringLiteral("tr")},
          {QStringLiteral("简体中文"), QStringLiteral("zh-cn")}};
}

static const ImWchar* QtHost::GetGlyphRangesJapanese()
{
  // clang-format off
  // auto update by generate_update_glyph_ranges.py with duckstation-qt_ja.ts
  static const char16_t chars[] = u"←↑→↓□△○　、。々「」〜あいうえおかがきぎくぐけげこごさざしじすずせそただちっつづてでとどなにぬねのはばびへべほぼぽまみむめもやゆよらりるれろわをんァアィイゥウェエォオカガキギクグケゲコゴサザシジスズセゼソタダチッツテデトドナニネノハバパヒビピフブプヘベペホボポマミムメモャヤュユョヨラリルレロワンー一上下不与両並中主了予事二互交人介他付代令以件任休伸位低体作使例供依係保信修個倍値停側傍備像優元先光入全公共具典内再凍処出分切初別利到制削前割力加効動勧化十協単去参及反収取古可右号各合同名向含告周呼命問善回囲固国圧在地垂型埋域基報場境増壊声売変外多大失奨妥始子字存学安完定宛実密対専射小少岐左差巻帰常幅平年度座延式引弱張強当形影役待後従得御復微心必忘応性恐情意感態成我戻所手扱投択押拡持指振挿排探接推描提換損摩撃撮操改敗数整文料断新方既日早明映時景更書替最有望期未本来析枚果栄検概構標権機欄次止正歪残毎比水永求汎決況法波注海消深混済減測源準滑演点無照版牲犠状獲率現理生用申画界番異疑発登的目直相瞬知短破確示禁秒称移程種穴空立端符等算管範簡粋精約純索細終組結統続維緑線編縮績繰置翻者耗背能自致般良色行表装補製複要見規視覧観解言計記設許訳証試詳認語説読調識警護象販費質赤起超跡転軸軽較込近返追送逆通速連進遅遊達遠適遷選部重野量録長閉開間関防降限除隅隠集離電青非面音響頂順領頭頻頼題類飛高鮮黒％？Ｘ";
  const int chars_length = sizeof(chars) / sizeof(chars[0]);
  // clang-format on

  static ImWchar base_ranges[] = {
    0x0020, 0x007E, // Basic Latin
  };
  const int base_length = sizeof(base_ranges) / sizeof(base_ranges[0]);

  static ImWchar full_ranges[base_length + chars_length * 2 + 1] = {0};
  memcpy(full_ranges, base_ranges, sizeof(base_ranges));
  for (int i = 0; i < chars_length; i++)
  {
    full_ranges[base_length + i * 2] = full_ranges[base_length + i * 2 + 1] = chars[i];
  }
  return full_ranges;
}

static const ImWchar* QtHost::GetGlyphRangesChinese()
{
  // clang-format off
  // auto update by generate_update_glyph_ranges.py with duckstation-qt_zh-cn.ts
  static const char16_t chars[] = u"“”…、。一丁三上下不与且丢两个中临为主么义之乐也了予事二于互亚些交亦产享人仅介从他代令以们件价任份仿休优会传伸但位低住体何余作你佳使例供依侧便保信修倍倒候值假偏停储像允充先光免入全公六共关其具典兼内册再写冲决况冻准减几出击函分切列则创初删利别到制刷前剔剩剪力功加务动助勾包化匹区十升半协卓单南占卡即卸压原去参叉及双反发取受变口只可台右号各合同名后向吗否含听启呈告周和哈响哪商善喜器噪回因围固国图圆圈在地场址坏坐块垂型域基堆填境增声处备复外多夜够大太失头夹奏好如始媒子孔字存它守安完宏官定实宫家容宽寄密察寸对寻导封射将小少尚尝尤就尺尼尽尾局层屏展属峰崩工左差已希带帧帮常幕平年并序库应底度延建开异弃式引张弦弱弹强当录形彩影彻征径很得心必忆志快忽态性总恢息您悬情想意慢懂戏成我或战截户所扇手才打执扩扫扭扳批找把抓投抖折护报抱抹拉拟择拷拿持指按挎挑振损换据捷排接控推描提插握搜携摇撤播操支收改放故效敏数整文斗料断新方施无日旦旧旨时明易星映是显景暂暗曲更替最有服望期未末本机权杆束条来板构析果枪某染查标栈栏校样核根格案档梦梳检概榜槽模横次欢欧欲歉止正此步死段每比毫水求汇池没法波注洲活派流浅测浏消涡深混添清渐渡渲游溃源滑滚滞滤澳激灰灵点炼热焦然照爆片版牌牙物特状独率玩环现理瑕生用由电画畅界留略疵登百的监盒盖盘目直相看真眠着瞬知矫码破硬确碎碰磁示禁离种秒称移程稍稳空突窗立站端笔第等筛签简算管类精系素索红约级纯纵纹线组细终经绑结绘给络统继绩续维绿缓编缘缩网置美翻者而耐耗联背能脑自致般色节若范荐荷获菜著蓝藏行补表衷被裁裂装要覆见观规视览觉角解触言警计订认议记许论设访证识译试该详语误说请读调象贝负败账质贴费资赖起超越足跃距跟跨路跳踪身轨转轮软轴载较辑输辨边达过运近返还这进远连迟述追退送适逆选透通速造遇道遵那邻部都配醒采释里重野量金针钮链锁锐错键锯镜长闭问间阈防阻降限除险隐隔障难集需震静非靠面音页顶项顺须顿预频题颜额风驱验高黑默鼠齿，：？";
  const int chars_length = sizeof(chars) / sizeof(chars[0]);
  // clang-format on

  static ImWchar base_ranges[] = {
    0x0020, 0x007E, // Basic Latin
  };
  const int base_length = sizeof(base_ranges) / sizeof(base_ranges[0]);

  static ImWchar full_ranges[base_length + chars_length * 2 + 1] = {0};
  memcpy(full_ranges, base_ranges, sizeof(base_ranges));
  for (int i = 0; i < chars_length; i++)
  {
    full_ranges[base_length + i * 2] = full_ranges[base_length + i * 2 + 1] = chars[i];
  }
  return full_ranges;
}
