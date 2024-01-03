// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "mainwindow.h"
#include "qthost.h"

#include "core/host.h"

#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include "imgui.h"

#include <QtCore/QFile>
#include <QtCore/QTranslator>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMessageBox>

#include <optional>
#include <vector>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

Log_SetChannel(QTTranslations);

#if 0
// Qt internal strings we'd like to have translated
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Services")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Hide %1")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Hide Others")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Show All")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Preferences...")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Quit %1")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "About %1")
#endif

namespace QtHost {
struct GlyphInfo
{
  const char* language;
  const char* imgui_font_name;
  const char* imgui_font_url;
  const char16_t* used_glyphs;
};

static QString FixLanguageName(const QString& language);
static void UpdateGlyphRangesAndClearCache(QWidget* dialog_parent, const std::string_view& language);
static bool DownloadMissingFont(QWidget* dialog_parent, const char* font_name, const char* font_url,
                                const std::string& path);
static const GlyphInfo* GetGlyphInfo(const std::string_view& language);

static constexpr const char* DEFAULT_IMGUI_FONT_NAME = "Roboto-Regular.ttf";
#define MAKE_FONT_DOWNLOAD_URL(name) "https://www.duckstation.org/runtime-resources/fonts/" name

static std::vector<QTranslator*> s_translators;
} // namespace QtHost

void QtHost::InstallTranslator(QWidget* dialog_parent)
{
  for (QTranslator* translator : s_translators)
  {
    qApp->removeTranslator(translator);
    translator->deleteLater();
  }
  s_translators.clear();

  // Fix old language names.
  const QString language =
    FixLanguageName(QString::fromStdString(Host::GetBaseStringSettingValue("Main", "Language", GetDefaultLanguage())));

  // install the base qt translation first
#ifndef __APPLE__
  const QString base_dir = QStringLiteral("%1/translations").arg(qApp->applicationDirPath());
#else
  const QString base_dir = QStringLiteral("%1/../Resources/translations").arg(qApp->applicationDirPath());
#endif

  // Qt base uses underscores instead of hyphens.
  const QString qt_language = QString(language).replace(QChar('-'), QChar('_'));
  QString base_path(QStringLiteral("%1/qt_%2.qm").arg(base_dir).arg(qt_language));
  bool has_base_ts = QFile::exists(base_path);
  if (!has_base_ts)
  {
    // Try without the country suffix.
    const int index = language.lastIndexOf('-');
    if (index > 0)
    {
      base_path = QStringLiteral("%1/qt_%2.qm").arg(base_dir).arg(language.left(index));
      has_base_ts = QFile::exists(base_path);
    }
  }
  if (has_base_ts)
  {
    QTranslator* base_translator = new QTranslator(qApp);
    if (!base_translator->load(base_path))
    {
      QMessageBox::warning(
        dialog_parent, QStringLiteral("Translation Error"),
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
      dialog_parent, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(language).arg(path));
    return;
  }

  QTranslator* translator = new QTranslator(qApp);
  if (!translator->load(path))
  {
    QMessageBox::warning(
      dialog_parent, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(language).arg(path));
    delete translator;
    return;
  }

  Log_InfoPrintf("Loaded translation file for language %s", language.toUtf8().constData());
  qApp->installTranslator(translator);
  s_translators.push_back(translator);

  // We end up here both on language change, and on startup.
  UpdateGlyphRangesAndClearCache(dialog_parent, language.toStdString());
}

QString QtHost::FixLanguageName(const QString& language)
{
  if (language == QStringLiteral("pt-br"))
    return QStringLiteral("pt-BR");
  else if (language == QStringLiteral("pt-pt"))
    return QStringLiteral("pt-PT");
  else if (language == QStringLiteral("zh-cn"))
    return QStringLiteral("zh-CN");
  else
    return language;
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view& context, const std::string_view& msg, char* tbuf,
                                            size_t tbuf_space)
{
  // This is really awful. Thankfully we're caching the results...
  const std::string temp_context(context);
  const std::string temp_msg(msg);
  const QString translated_msg = qApp->translate(temp_context.c_str(), temp_msg.c_str());
  const QByteArray translated_utf8 = translated_msg.toUtf8();
  const size_t translated_size = translated_utf8.size();
  if (translated_size > tbuf_space)
    return -1;
  else if (translated_size > 0)
    std::memcpy(tbuf, translated_utf8.constData(), translated_size);

  return static_cast<s32>(translated_size);
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  static constexpr const std::pair<const char*, const char*> languages[] = {{"English", "en"},
                                                                            {"Deutsch", "de"},
                                                                            {"Español de Latinoamérica", "es"},
                                                                            {"Español de España", "es-ES"},
                                                                            {"Français", "fr"},
                                                                            {"עברית", "he"},
                                                                            {"Bahasa Indonesia", "id"},
                                                                            {"日本語", "ja"},
                                                                            {"한국어", "ko"},
                                                                            {"Italiano", "it"},
                                                                            {"Nederlands", "nl"},
                                                                            {"Polski", "pl"},
                                                                            {"Português (Pt)", "pt-PT"},
                                                                            {"Português (Br)", "pt-BR"},
                                                                            {"Русский", "ru"},
                                                                            {"Türkçe", "tr"},
                                                                            {"简体中文", "zh-CN"}};

  return languages;
}

bool Host::ChangeLanguage(const char* new_language)
{
  QtHost::RunOnUIThread([new_language = std::string(new_language)]() {
    Host::SetBaseStringSettingValue("Main", "Language", new_language.c_str());
    Host::CommitBaseSettingChanges();
    QtHost::InstallTranslator(g_main_window);
    g_main_window->recreate();
  });
  return true;
}

const char* QtHost::GetDefaultLanguage()
{
  // TODO: Default system language instead.
  return "en";
}

static constexpr const ImWchar s_base_latin_range[] = {
  0x0020, 0x00FF, // Basic Latin + Latin Supplement
};
static constexpr const ImWchar s_central_european_ranges[] = {
  0x0100, 0x017F, // Central European diacritics
};

void QtHost::UpdateGlyphRangesAndClearCache(QWidget* dialog_parent, const std::string_view& language)
{
  const GlyphInfo* gi = GetGlyphInfo(language);

  const char* imgui_font_name = nullptr;
  const char* imgui_font_url = nullptr;
  std::vector<ImWchar> glyph_ranges;
  glyph_ranges.clear();

  // Base Latin range is always included.
  glyph_ranges.insert(glyph_ranges.begin(), std::begin(s_base_latin_range), std::end(s_base_latin_range));

  if (gi)
  {
    if (gi->used_glyphs)
    {
      const char16_t* ptr = gi->used_glyphs;
      while (*ptr != 0)
      {
        // Always should be in pairs.
        DebugAssert(ptr[0] != 0 && ptr[1] != 0);
        glyph_ranges.push_back(*(ptr++));
        glyph_ranges.push_back(*(ptr++));
      }
    }

    imgui_font_name = gi->imgui_font_name;
    imgui_font_url = gi->imgui_font_url;
  }

  // If we don't have any specific glyph range, assume Central European, except if English, then keep the size down.
  if ((!gi || !gi->used_glyphs) && language != "en")
  {
    glyph_ranges.insert(glyph_ranges.begin(), std::begin(s_central_european_ranges),
                        std::end(s_central_european_ranges));
  }

  // List terminator.
  glyph_ranges.push_back(0);
  glyph_ranges.push_back(0);

  // Check for the presence of font files.
  std::string font_path;
  if (imgui_font_name)
  {
    DebugAssert(imgui_font_url);

    // Non-standard fonts always go to the user resources directory, since they're downloaded on demand.
    font_path = Path::Combine(EmuFolders::UserResources,
                              SmallString::from_format("fonts" FS_OSPATH_SEPARATOR_STR "{}", imgui_font_name));
    if (!DownloadMissingFont(dialog_parent, imgui_font_name, imgui_font_url, font_path))
      font_path.clear();
  }
  if (font_path.empty())
  {
    // Use the default font.
    font_path = EmuFolders::GetOverridableResourcePath(
      SmallString::from_format("fonts" FS_OSPATH_SEPARATOR_STR "{}", DEFAULT_IMGUI_FONT_NAME));
  }

  if (g_emu_thread)
  {
    Host::RunOnCPUThread([font_path = std::move(font_path), glyph_ranges = std::move(glyph_ranges)]() mutable {
      ImGuiManager::SetFontPathAndRange(std::move(font_path), std::move(glyph_ranges));
      Host::ClearTranslationCache();
    });
  }
  else
  {
    // Startup, safe to set directly.
    ImGuiManager::SetFontPathAndRange(std::move(font_path), std::move(glyph_ranges));
    Host::ClearTranslationCache();
  }
}

bool QtHost::DownloadMissingFont(QWidget* dialog_parent, const char* font_name, const char* font_url,
                                 const std::string& path)
{
  if (FileSystem::FileExists(path.c_str()))
    return true;

  {
    QMessageBox msgbox(dialog_parent);
    msgbox.setWindowTitle(qApp->translate("QtHost", "Missing Font File"));
    msgbox.setWindowIcon(QtHost::GetAppIcon());
    msgbox.setIcon(QMessageBox::Critical);
    msgbox.setTextFormat(Qt::RichText);
    msgbox.setText(
      qApp
        ->translate(
          "QtHost",
          "The font file '%1' is required for the On-Screen Display and Big Picture Mode to show messages in your "
          "language.<br><br>"
          "Do you want to download this file now? These files are usually less than 10 megabytes in size.<br><br>"
          "<strong>If you do not download this file, on-screen messages will not be readable.</strong>")
        .arg(QLatin1StringView(font_name)));
    msgbox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    if (msgbox.exec() != QMessageBox::Yes)
      return false;
  }

  const QString progress_title = qApp->translate("QtHost", "Downloading Files");
  if (StringUtil::EndsWithNoCase(font_url, ".zip"))
    return QtHost::DownloadFileFromZip(dialog_parent, progress_title, font_url, font_name, path.c_str());
  else
    return QtHost::DownloadFile(dialog_parent, progress_title, font_url, path.c_str());
}

// clang-format off
static constexpr const char16_t s_cyrillic_ranges[] = {
  /* Cyrillic + Cyrillic Supplement */ 0x0400, 0x052F, /* Extended-A */ 0x2DE0, 0x2DFF, /* Extended-B */ 0xA640, 0xA69F, 0, 0
};
static constexpr const QtHost::GlyphInfo s_glyph_info[] = {
  // Cyrillic languages
  { "ru", nullptr, nullptr, s_cyrillic_ranges },
  { "sr", nullptr, nullptr, s_cyrillic_ranges },
  { "uk", nullptr, nullptr, s_cyrillic_ranges },

  {
    "ja", "NotoSansJP-Regular.ttf", MAKE_FONT_DOWNLOAD_URL("NotoSansJP-Regular.zip"),
    // auto update by generate_update_glyph_ranges.py with duckstation-qt_ja.ts
    u"←↓□□△△○○　。々々「」〜〜ああいいううええおせそそたちっばびびへべほもややゆゆよろわわをんァソタチッツテニネロワワンンーー一一上下不与両両並並中中主主了了予予事二互互交交人人今介他他付付代以件件任任休休伸伸位低体体作作使使例例供供依依価価係係保保信信修修個個倍倍値値停停側側傍傍備備像像優優元元先光入入全全公公共共具典内内再再凍凍処処出出分切初初別別利利到到制制削削前前割割力力加加効効動動勧勧化化十十協協単単去去参参及及反反取取古古可可右右号号各各合合同名向向含含告告周周命命問問善善回回囲囲固固国国圧在地地垂垂型型域域基基報報場場境境増増壊壊声声売売変変外外多多大大央央失失奨奨妥妥始始子子字存学学安安完完定宛実実密密対対専専射射小小少少岐岐左左差差巻巻帰帰常常幅幅平年度座延延式式引引弱弱張張強強当当形形影影役役待待後後従従得得御御復復微微心心必必忘忘応応性性恐恐情情意意感感態態成我戻戻所所手手扱扱投投択択押押拡拡持持指指振振排排探探接接推推描提換換損損摩摩撃撃撮撮操操改改敗敗数数整整文文料料断断新新方方既既日日早早明明昔昔映映昨昨時時景景更更書書替最有有望望期期未未本本来来析析枚枚果果枠枠栄栄検検概概構構標標権権機機欄欄次次止正歪歪残残毎毎比比水水永永求求汎汎決決況況法法波波注注海海消消深深混混済済減減測測源源準準滑滑演演点点無無照照版版牲牲特特犠犠状状率率現現理理生生用用申申画画界界番番異異疑疑発登的的目目直直瞬瞬知知短短破破確確示示禁禁秒秒移移程程種種穴穴空空立立端端符符等等算算管管範範簡簡粋粋精精約約純純索索細細終終組組結結統統続続維維緑緑線線編編縮縮績績繰繰置置翻翻者者耗耗背背能能自自致致般般良良色色荷荷行行表表装装補補製製複複要要見見規規視視覧覧観観解解言言計計記記設設許許訳訳証証試試詳詳認認語語説読調調識識警警象象負負販販費費質質赤赤起起超超跡跡転転軸軸軽軽較較込込近近返返追追送送逆逆通通速速連連進進遅遅遊遊達達遠遠適適遷選部部重量録録長長閉閉開開間間関関防防降降限限除除隅隅隠隠集集離離電電青青非非面面音音響響頂頂順順領領頭頭頻頼題題類類飛飛高高黒黒％％？？ＸＸ"
  },
  {
    "ko", "NotoSansKR-Regular.ttf", MAKE_FONT_DOWNLOAD_URL("NotoSansKR-Regular.zip"),
    // auto update by generate_update_glyph_ranges.py with duckstation-qt_ko.ts
    u"←↓□□△△○○◯◯。。んんイイジジメメーー茶茶가각간간감값갔강같같개개갱갱거거건건걸걸검겁것것게게겠겠겨격견견결결경경계계고고곤곤곳곳공공과곽관관괴괴교교구국권권귀귀규규그그근근글글금급기기긴긴길길김깁깅깅깊깊까까깝깝깨깨꺼꺼께께꽂꽂꿉꿉끄끄끊끊끔끔나나날날낮낮내내너너널널넘넘네네넷넷높놓누누눈눈눌눌뉴뉴느느는는늘늘능능니니닌닌님닙다다단단닫달당당대대댑댑더더덜덜덤덤덮덮데데델델뎁뎁도도돌돌동동되되된된될될됨됩두두둔둔둘둘뒤뒤듀듀드득든든들들듭듭등등디디딩딩따따때때떠떠떤떤떨떨떻떻또또뚜뚜뛰뛰뛸뛸뜨뜨뜻뜻띄띄라라란란랍랍랑랑래랙랜랜램램랫랫량량러럭런런럼럽렀렀렇렉렌렌려력련련렬렬렷렷령령로록론론롤롤롭롭롯롯료료루루룹룹류류률률륨륨르르른른를를름릅리릭린린릴릴림립릿릿링링마막만만많많말말맞맞매매머머먼먼멀멀멈멈멋멋멍멍메메멘멘며며면면명명몇몇모목못못무무문문물물뭉뭉뮬뮬므므미미밀밀밍밍및및바박반반받밝방방배백버벅번번벌벌범법벗벗베베벤벤벨벨변변별별병병보복본본볼볼봉봉부부분분불불뷰뷰브브블블비빅빈빈빌빌빗빗빛빛빠빠빨빨뿐뿐사삭산산삼삽상상새색샘샘생생샤샤샷샷서석선선설설성성세섹센센셀셀셈셈셋셋션션셰셰소속손손솔솔송송쇼쇼수수순순숨숨슈슈스스슬슬습습승승시식신신실실심십싱싱싶싶쌍쌍써써썰썰쓰쓰씁씁씌씌씬씬아악안안않않알알암압았앙애액앤앤앨앨야약양양어어언언얻얼업없었었에에엔엔여역연연열열영영예예오오온온올올옵옵와와완완왑왑왔왔외외왼왼요요용용우욱운운움웁웃웃워워원원웨웨위위유유율율으으은은을을음음응응의의이이인인일읽임입있있잊잊자작잘잘잠잡장장재재잿잿저적전전절절점접정정제젝젠젠젯젯져져졌졌조족존존종종좋좌죄죄주주준준줄줄줍줍중중즈즉즐즐증증지직진진질질짐집째째쪽쪽찍찍차착참참창찾채채처처천천청청체체쳐쳐초초총총촬촬최최추축출출충충춰춰취취츠측치치침침카카캐캐캔캔커커컨컨컬컬컴컴케케켤켤코코콘콘콜콜큐큐크크큰큰클클큼큼키키킬킬킵킵킹킹타타탄탄탐탐태택탬탭터터턴턴털털테텍텐텐템텝토토통통투투트특튼튼틀틀티틱틴틴틸틸팅팅파파팝팝패패퍼퍼페페편편평평폐폐포폭폴폴폼폼표표푸푸풀풀품품퓨퓨프프픈픈플플피픽필필핑핑하학한한할할함합핫핫항항해핵했행향향허허험험헤헤현현형형호혹혼혼화확환환활활황황회획횟횟횡횡효효후후훨훨휘휘휴휴흔흔희희히히힙힙ＸＸ"
  },
  {
    "zh-CN", "NotoSansSC-Regular.ttf", MAKE_FONT_DOWNLOAD_URL("NotoSansSC-Regular.zip"),
    // auto update by generate_update_glyph_ranges.py with duckstation-qt_zh-cn.ts
    u"——“”……、。一丁三下不与专且丢丢两两个个中中临临为主么义之之乐乐乘乘也也了了事二于于互互亚些交交亦产人人仅仅今介仍从仓仓他他付付代以们们件价任任份份仿仿休休众优会会传传伸伸但但位住体体何何作作佳佳使使例例供供依依侧侧便便保保信信修修倍倍倒倒候候借借值值假假偏偏停停储储像像允允元元充兆先光免免入入全全公六共共关关其典兼兼内内册再写写冲决况况冻冻准准减减几几凭凭出击函函分切列列则则创创初初删删利利别别到到制刷前前剔剔剪剪力力功务动助勾勾包包化化匹区十十升升半半协协卓卓单单南南占卡即即历历压压原原去去参参叉及双反发发取变口口另另只只可台史右号司各各合合同后向向吗吗否否含听启启呈呈告告员员周周味味命命和和咫咫哈哈响响哪哪商商善善器器噪噪回回因因困困围围固固国图圆圆圈圈在在地地场场址址均均坏坐块块垂垂型型域域基基堆堆填填增增声声处处备备复复外外多多夜夜够够大大天太失失头头夹夹奏奏好好如如妙妙始始娱娱媒媒子子孔孔字存它它安安完完宏宏官官定定实实家家容容宽宽寄寄密密察察寸对寻导寿寿封封射射将将小小少少尝尝尤尤就就尺尺尼尽局局层层屏屏展展属属峰峰崩崩工左差差己已希希带帧帮帮常常幕幕平年并并序序库底度度延延建建开开异弃式式引引张张弦弦弱弱弹强当当录录形形彩彩影影径待很很律律得得微微心心必忆志志快快忽忽态态性性总总恢恢息息您您悬悬情情惯惯想想愉愉意意慢慢戏我或或战战截截戳戳户户所所扇扇手手才才打打托托执执扫扭批批找找技技把把投抖折折护报抱抱抹抹拉拉拟拟拥拥择择括括拷拷拾拿持挂指指按按挑挑振振损损换换据据掉掉掌掌排排接接控掩描提插插握握搜搜携携摇摇撤撤播播操操支支改改放放故故效效敏敏数数整整文文料料断断新新方方无无日旧早早时时明明星映昨昨是是显显晚晚景景暂暂暗暗曲曲更更替最有有服服望望期期未本术术机机权权杆杆束束条条来来板板构构析析果果枪枪柄柄某某染染查查标栈栏栏校校样根格格框框案案档档械械梳梳检检概概榜榜模模橇橇次欢欧欧歉歉止步死死段段每每比比毫毫水水求求汇汇池池没没法法注注洲洲活活流流浅浅测测浏浏消消涡涡深深混混添添清清渐渐渡渡渲渲游游溃溃源源滑滑滚滚滞滞滤滤演演潜潜澳澳激激灰灰灵灵点点炼炼热热焦焦然然照照片版牌牌牙牙物物特特状状独独献献率率玩玩现现理理瓶瓶生生用用由由电电画画畅畅界界留留略略登登百百的的监盒盖盖盘盘目目直直相相看看真眠着着瞄瞄瞬瞬知知矫矫码码破破硬硬确确磁磁示示禁禁离离种种秒秒称称移移程程稍稍稳稳空空突突窗窗立立端端符符第第等等筛筛签签简简算算管管类类精精系系素素索索红红约级纯纯纵纵纹纹线线组组细细终终经经绑绑结结绘给络络统统继继续续维维绿绿缓缓编编缘缘缩缩缺缺网网置置美美翻翻者者而而耐耐联联背背能能脑脑自自致致航航般般色色节节若若范范荐荐获获菜菜著著蓝蓝藏藏虚虚行行补补表表衷衷被被裁裂装装要要覆覆见观规规视视览觉角角解解触触言言警警计订认认议议记记许许论论设访证证评评识识译译试试话话该详语语误误说说请诸读读调调贝贝负负贡贡败账质质贴贴费费资资赖赖起起超超越越足足跟跟跨跨路路跳跳踪踪身身车车轨轨转转轮软轴轴载载较较辑辑输输辨辨边边达达过过迎迎运近返返还这进远连迟述述追追退适逆逆选选透透通通速速遇遇道道遥遥避避那那邻邻部部都都配配醒醒采采释释里量金金针针钟钟链链锁锁锐锐错错键锯镜镜长长闭问间间阈阈防防阻阻附附降降限限除除隆隆随隐隔隔障障隶隶难难集集需需震震静静非非靠靠面面音音页顶项须顿顿预预颈颈频频题题颜额颠颠饱饱驱驱验验骤骤高高鸭鸭黑黑默默鼠鼠齐齐齿齿！！％％，，：：？？"
  },
};
// clang-format on

const QtHost::GlyphInfo* QtHost::GetGlyphInfo(const std::string_view& language)
{
  for (const GlyphInfo& it : s_glyph_info)
  {
    if (language == it.language)
      return &it;
  }

  return nullptr;
}