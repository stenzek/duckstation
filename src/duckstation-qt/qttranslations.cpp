// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "qthost.h"

#include "core/host.h"

#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include "imgui.h"

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
  const char* windows_font_name;
  const char* linux_font_name;
  const char* mac_font_name;
  const char16_t* used_glyphs;
};

static std::string GetFontPath(const GlyphInfo* gi);
static void UpdateGlyphRanges(const std::string_view& language);
static const GlyphInfo* GetGlyphInfo(const std::string_view& language);

static std::vector<ImWchar> s_glyph_ranges;
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

  UpdateGlyphRanges(language.toStdString());

  Host::ClearTranslationCache();
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

static std::string QtHost::GetFontPath(const GlyphInfo* gi)
{
  std::string font_path;

#ifdef _WIN32
  if (gi->windows_font_name)
  {
    PWSTR folder_path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &folder_path)))
    {
      font_path = StringUtil::WideStringToUTF8String(folder_path);
      CoTaskMemFree(folder_path);
      font_path += "\\";
      font_path += gi->windows_font_name;
    }
    else
    {
      font_path = fmt::format("C:\\Windows\\Fonts\\{}", gi->windows_font_name);
    }
  }
#elif defined(__APPLE__)
  if (gi->mac_font_name)
    font_path = fmt::format("/System/Library/Fonts/{}", gi->mac_font_name);
#endif

  return font_path;
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
          {QStringLiteral("한국어"), QStringLiteral("ko")},
          {QStringLiteral("Italiano"), QStringLiteral("it")},
          {QStringLiteral("Nederlands"), QStringLiteral("nl")},
          {QStringLiteral("Polski"), QStringLiteral("pl")},
          {QStringLiteral("Português (Pt)"), QStringLiteral("pt-pt")},
          {QStringLiteral("Português (Br)"), QStringLiteral("pt-br")},
          {QStringLiteral("Русский"), QStringLiteral("ru")},
          {QStringLiteral("Türkçe"), QStringLiteral("tr")},
          {QStringLiteral("简体中文"), QStringLiteral("zh-cn")}};
}

static constexpr const ImWchar s_base_latin_range[] = {
  0x0020, 0x00FF, // Basic Latin + Latin Supplement
};
static constexpr const ImWchar s_central_european_ranges[] = {
  0x0100, 0x017F, // Central European diacritics
};

void QtHost::UpdateGlyphRanges(const std::string_view& language)
{
  const GlyphInfo* gi = GetGlyphInfo(language);

  std::string font_path;
  s_glyph_ranges.clear();

  // Base Latin range is always included.
  s_glyph_ranges.insert(s_glyph_ranges.begin(), std::begin(s_base_latin_range), std::end(s_base_latin_range));

  if (gi)
  {
    if (gi->used_glyphs)
    {
      const char16_t* ptr = gi->used_glyphs;
      while (*ptr != 0)
      {
        // Always should be in pairs.
        DebugAssert(ptr[0] != 0 && ptr[1] != 0);
        s_glyph_ranges.push_back(*(ptr++));
        s_glyph_ranges.push_back(*(ptr++));
      }
    }

    font_path = GetFontPath(gi);
  }

  // If we don't have any specific glyph range, assume Central European, except if English, then keep the size down.
  if ((!gi || !gi->used_glyphs) && language != "en")
  {
    s_glyph_ranges.insert(s_glyph_ranges.begin(), std::begin(s_central_european_ranges),
                          std::end(s_central_european_ranges));
  }

  // List terminator.
  s_glyph_ranges.push_back(0);
  s_glyph_ranges.push_back(0);

  ImGuiManager::SetFontPath(std::move(font_path));
  ImGuiManager::SetFontRange(s_glyph_ranges.data());
}

// clang-format off
static constexpr const char16_t s_cyrillic_ranges[] = {
  /* Cyrillic + Cyrillic Supplement */ 0x0400, 0x052F, /* Extended-A */ 0x2DE0, 0x2DFF, /* Extended-B */ 0xA640, 0xA69F, 0, 0
};
static constexpr const QtHost::GlyphInfo s_glyph_info[] = {
  // Cyrillic languages
  { "ru", nullptr, nullptr, nullptr, s_cyrillic_ranges },
  { "sr", nullptr, nullptr, nullptr, s_cyrillic_ranges },
  { "uk", nullptr, nullptr, nullptr, s_cyrillic_ranges },

  {
    "ja", "msgothic.ttc", nullptr, "ヒラギノ角ゴシック W3.ttc",
    // auto update by generate_update_glyph_ranges.py with duckstation-qt_ja.ts
    u"←↓□□△△○○　。々々「」〜〜ああいいううええおせそそたちっばびびへべほもややゆゆよろわわをんァソタチッツテニネロワワンンーー一一上下不与両両並並中中主主了了予予事二互互交交人人今介他他付付代以件件任任休休伸伸位低体体作作使使例例供供依依価価係係保保信信修修個個倍倍値値停停側側傍傍備備像像優優元元先光入入全全公公共共具典内内再再凍凍処処出出分切初初別別利利到到制制削削前前割割力力加加効効動動勧勧化化十十協協単単去去参参及及反収取取古古可可右右号号各各合合同名向向含含告告周周呼命問問善善回回囲囲固固国国圧在地地垂垂型型埋埋域域基基報報場場境境増増壊壊声声売売変変外外多多大大央央失失奨奨妥妥始始子子字存学学安安完完定宛実実密密対対専専射射小小少少岐岐左左差差巻巻帰帰常常幅幅平年度座延延式式引引弱弱張張強強当当形形影影役役待待後後従従得得御御復復微微心心必必忘忘応応性性恐恐情情意意感感態態成我戻戻所所手手扱扱投投択択押押拡拡持持指指振振挿挿排排探探接接推推描提換換損損摩摩撃撃撮撮操操改改敗敗数数整整文文料料断断新新方方既既日日早早明明昔昔映映昨昨時時景景更更書書替最有有望望期期未未本本来来析析枚枚果果枠枠栄栄検検概概構構標標権権機機欄欄次次止正歪歪残残毎毎比比水水永永求求汎汎決決況況法法波波注注海海消消深深混混済済減減測測源源準準滑滑演演点点無無照照版版牲牲特特犠犠状状獲獲率率現現理理生生用用申申画画界界番番異異疑疑発登的的目目直直相相瞬瞬知知短短破破確確示示禁禁秒秒称称移移程程種種穴穴空空立立端端符符等等算算管管範範簡簡粋粋精精約約純純索索細細終終組組結結統統続続維維緑緑線線編編縮縮績績繰繰置置翻翻者者耗耗背背能能自自致致般般良良色色荷荷行行表表装装補補製製複複要要見見規規視視覧覧観観解解言言計計記記設設許許訳訳証証試試詳詳認認語語説読調調識識警警護護象象負負販販費費質質赤赤起起超超跡跡転転軸軸軽軽較較込込近近返返追追送送逆逆通通速速連連進進遅遅遊遊達達遠遠適適遷選部部重量録録長長閉閉開開間間関関防防降降限限除除隅隅隠隠集集離離電電青青非非面面音音響響頂頂順順領領頭頭頻頼題題類類飛飛高高鮮鮮黒黒％％？？ＸＸ"
  },
  {
    "ko", "malgun.ttf", nullptr, "AppleSDGothicNeo.ttc",
    // auto update by generate_update_glyph_ranges.py with duckstation-qt_ko.ts
    u"←↓□□△△○○◯◯。。ささししたたににははままれれんんイイククジジトトメメンンーー成成生生茶茶가각간간감값갔강같같개개갱갱거거건건걸걸검검것것게게겠겠겨겨견견결결경경계계고고곤곤곳곳공공과곽관관괴괴교교구국권권귀귀규규그그근근글글금급기기긴긴길길김깁깅깅깊깊까까깝깝깨깨꺼꺼께께꽂꽂끄끄끊끊끔끔나나날날낮낮내내너너널널넘넘네네넷넷높놓누누눈눈눌눌뉴뉴느느는는늘늘능능니니닌닌닙닙다다단단닫달당당대대댑댑더더덜덜덤덤덮덮데데델델도도돌돌동동되되된된될될됨됩두두둔둔둘둘뒤뒤듀듀드득든든들들등등디디딩딩따따때때떠떠떤떤떨떨떻떻또또뚜뚜뛰뛰뛸뛸뜨뜨뜻뜻띄띄라라란란랑랑래랙랜랜램램랫랫량량러럭런런럼럽렀렀렇렉렌렌려력련련렬렬렷렷령령로록론론롤롤롭롭롯롯료료루루룹룹류류률률륨륨르르른른를를름릅리릭린린릴릴림립릿릿링링마막만만많많말말맞맞매매머머먼먼멀멀멈멈멋멋멍멍메메멘멘멤멤며며면면명명몇몇모목못못무무문문물물뭉뭉뮬뮬므므미미밀밀밍밍및및바박반반받발방방배백버벅번번범법벗벗베베벤벤벨벨변변별별병병보복본본볼볼부부분분불불뷰뷰브브블블비빅빈빈빌빌빗빗빛빛빠빠빨빨뿐뿐사삭산산삽삽상상새색샘샘생생샤샤샷샷서석선선설설성성세섹센센셀셀셈셈셋셋션션셰셰소속손손솔솔송송쇼쇼수수순순숨숨슈슈스스슬슬습습시식신신실실심십싱싱쌍쌍써써썰썰쓰쓰씁씁씌씌씬씬아악안안않않알알압압았앙애액앤앤앨앨야약양양어어언언얻얼업없었었에에엔엔엣엣여역연연열열영영예예오오올올옵옵와와완완왑왑왔왔외외왼왼요요용용우우운운움웁웃웃워워원원웨웨위위유유율율으으은은을을음음응응의의이이인인일읽잃입있있잊잊자작잘잘잠잡장장재재잿잿저적전전절절점접정정제젝젠젠젯젯져져졌졌조족존존종종좋좌죄죄주주준준줄줄줍줍중중즈즉증증지직진진질질짐집째째쪽쪽찍찍차차참참창찾채채처처천천첫첫청청체체쳐쳐초초총총최최추축출출충충취취츠측치치침침칭칭카카캐캐캔캔커커컨컨컬컬컴컴케케켤켤코코콘콘콜콜크크큰큰클클큼큼키키킬킬킵킵킹킹타타탐탐태택탭탭터터턴턴털털테텍텐텐템텝토토통통투투트특튼튼틀틀티틱틴틴팅팅파파팝팝패패퍼퍼페페편편평평포폭폴폴폼폼표표푸푸품품풍풍퓨퓨프프픈픈플플피픽필필핑핑하학한한할할함합핫핫항항해핵했행향향허허현현형형호호혼혼화확환환활활황황회획횟횟횡횡효효후후훨훨휘휘휴휴흔흔히히힙힙ＸＸ"
  },
  {
    "zh-cn", "msyh.ttc", nullptr, "Hiragino Sans GB.ttc",
    // auto update by generate_update_glyph_ranges.py with duckstation-qt_zh-cn.ts
    u"“”……、。一丁三下不与且且丢丢两两个个中中临临为主么义之之乐乐也也了了予予事二于于互互亚些交交亦产享享人人仅仅今介仍从他他付付代以们们件价任任份份仿仿休休优优会会传传伸伸但但位住体体何何余余作作佳佳使使例例供供依依侧侧便便保保信信修修倍倍倒倒候候值值假假偏偏停停储储像像允允充充先光免免入入全全公六共共关关其典兼兼内内册再写写冲决况况冻冻准准减减几几出击函函分切列列则则创创初初删删利利别别到到制刷前前剔剔剩剪力力功务动助勾勾包包化化匹区十十升升半半协协卓卓单单南南占卡即即卸卸压压原原去去参参叉及双反发发取变口口只只可台右右号号各各合合同后向向吗吗否否含听启启呈呈告告周周味味命命和和哈哈响响哪哪商商善善喜喜器器噪噪回回因因困困围围固固国图圆圆圈圈在在地地场场址址坏坐块块垂垂型型域域基基堆堆填填境境增增声声处处备备复复外外多多夜夜够够大大天太失失头头夹夹奏奏好好如如始始媒媒子子孔孔字存它它守安完完宏宏官官定定实实宫宫家家容容宽宽寄寄密密察察寸对寻导封封射射将将小小少少尚尚尝尝尤尤就就尺尺尼尾局局层层屏屏展展属属峰峰崩崩工左差差己已希希带帧帮帮常常幕幕平年并并序序库底度度延延建建开开异弃式式引引张张弦弦弱弱弹强当当录录形形彩彩影影彻彻征征径待很很得得心心必忆志志快快忽忽态态性性总总恢恢息息您您悬悬情情惯惯想想意意慢慢懂懂戏我或或战战截截户户所所扇扇手手才才打打执执扩扩扫扫扭扭扳扳批批找找把把抓抓投抖折折护报抱抱抹抹拉拉拟拟择择括括拷拷拿拿持持指指按按挎挎挑挑振振损损换换据据捷捷排排接接控推描提插插握握搜搜携携摇摇撤撤播播操操支支收收改改放放故故效效敏敏数数整整文文斗斗料料断断新新方方施施无无日旨时时明明易易星映昨昨是是显显景景暂暂暗暗曲曲更更替最有有服服望望期期未本机机权权杆杆束束条条来来板板构构析析果果枪枪某某染染查查标栈栏栏校校样根格格框框案案档档梦梦梳梳检检概概榜榜槽槽模模横横次欢欧欧欲欲歉歉止步死死段段每每比比毫毫水水求求汇汇池池没没法法波波注注洲洲活活派派流流浅浅测测浏浏消消涡涡深深混混添添清清渐渐渡渡渲渲游游溃溃源源滑滑滚滚滞滞滤滤潜潜澳澳激激灰灰灵灵点点炼炼热热焦焦然然照照爆爆片版牌牌牙牙物物特特状状独独率率玩玩环现理理瑕瑕瓶瓶生生用用由由电电画画畅畅界界留留略略疵疵登登百百的的监盒盖盖盘盘目目直直相相看看真眠着着瞬瞬瞰瞰知知矫矫码码破破硬硬确确碎碎碰碰磁磁示示禁禁离离种种秒秒称称移移程程稍稍稳稳空空突突窗窗立立站站端端笔笔第第等等筛筛签签简简算算管管类类精精系系素素索索红红约级纯纯纵纵纹纹线线组组细细终终经经绑绑结结绘给络络统统继继绩绩续续维维绿绿缓缓编编缘缘缩缩网网置置美美翻翻者者而而耐耐耗耗联联背背能能脑脑自自致致般般色色节节若若范范荐荐获获菜菜著著蓝蓝藏藏虚虚行行补补表表衷衷被被裁裂装装要要覆覆见观规规视视览觉角角解解触触言言警警计订认认议议记记许许论论设访证证识识译译试试该详语语误误说说请诸读读调调象象贝贝负负败账质质贴贴费费资资赖赖起起超超越越足足跃跃距距跟跟跨跨路路跳跳踪踪身身轨轨转转轮软轴轴载载较较辑辑输输辨辨边边达达过过运近返返还这进远连迟述述追追退适逆逆选选透透通通速造遇遇道道遥遥遵遵避避那那邻邻部部都都配配醒醒采采释释里量金金针针钟钟钮钮链链锁锁锐锐错错键锯镜镜长长闭问间间阈阈防防阻阻降降限限除除险险随隐隔隔障障难难集集需需震震静静非非靠靠面面音音页顶项须顿顿预预颈颈频频题题颜额风风驱驱验验高高黑黑默默鼠鼠齿齿，，：：？？"
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