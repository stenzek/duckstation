// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "bios.h"
#include "core.h"
#include "cpu_disasm.h"
#include "mips_encoder.h"
#include "settings.h"

#include "util/translation.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
#include "common/string_util.h"

LOG_CHANNEL(BIOS);

namespace BIOS {

static constexpr ImageInfo::Hash MakeHashFromString(const char str[])
{
  return StringUtil::ParseFixedHexString<ImageInfo::HASH_SIZE>(str);
}

// clang-format off
// Launch console BIOS is de-prioritized due to bugs.
// Late PAL is de-prioritized due to additional regional checks that break import booting without fast boot.
// PS2 is de-prioritized due to requiring a dynamic fast boot patch.
// PS2 PAL is further de-prioritized due to additional region checks.
static constexpr const ImageInfo s_image_info_by_hash[] = {
  {"SCPH-1000, DTL-H1000 (v1.0)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 50, MakeHashFromString("239665b1a3dade1b5a52c06338011044")},
  {"SCPH-1001, 5003, DTL-H1201, H3001 (v2.2 12-04-95 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("924e392ed05558ffdb115408c263dccf")},
  {"SCPH-1002, DTL-H1002 (v2.0 05-10-95 E)", ConsoleRegion::PAL, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("54847e693405ffeb0359c6287434cbef")},
  {"SCPH-1002, DTL-H1102 (v2.1 07-17-95 E)", ConsoleRegion::PAL, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("417b34706319da7cf001e76e40136c23")},
  {"SCPH-1002, DTL-H1202, H3002 (v2.2 12-04-95 E)", ConsoleRegion::PAL, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("e2110b8a2b97a8e0b857a45d32f7e187")},
  {"DTL-H1100 (v2.2 03-06-96 D)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 20, MakeHashFromString("ca5cfc321f916756e3f0effbfaeba13b")},
  {"SCPH-3000, DTL-H1000H (v1.1 01-22-95)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("849515939161e62f6b866f6853006780")},
  {"SCPH-1001, DTL-H1001 (v2.0 05-07-95 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("dc2b9bf8da62ec93e868cfd29f0d067d")},
  {"SCPH-3500 (v2.1 07-17-95 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("cba733ceeff5aef5c32254f1d617fa62")},
  {"SCPH-1001, DTL-H1101 (v2.1 07-17-95 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("da27e8b6dab242d8f91a9b25d80c63b8")},
  {"SCPH-5000, DTL-H1200, H3000 (v2.2 12-04-95 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 5, MakeHashFromString("57a06303dfa9cf9351222dfcbb4a29d9")},
  {"SCPH-5500 (v3.0 09-09-96 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 5, MakeHashFromString("8dd7d5296a650fac7319bce665a6a53c")},
  {"SCPH-5501, 5503, 7003 (v3.0 11-18-96 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 5, MakeHashFromString("490f666e1afb15b7362b406ed1cea246")},
  {"SCPH-5502, 5552 (v3.0 01-06-97 E)", ConsoleRegion::PAL, false, ImageInfo::FastBootPatch::Type1, 5, MakeHashFromString("32736f17079d0b2b7024407c39bd3050")},
  {"SCPH-7000, 7500, 9000 (v4.0 08-18-97 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("8e4c14f567745eff2f0408c8129f72a6")},
  {"SCPH-7000W (v4.1 11-14-97 A)", ConsoleRegion::NTSC_J, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("b84be139db3ee6cbd075630aa20a6553")},
  {"SCPH-7001, 7501, 7503, 9001, 9003, 9903 (v4.1 12-16-97 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("1e68c231d0896b7eadcad1d7d8e76129")},
  {"SCPH-7002, 7502, 9002 (v4.1 12-16-97 E)", ConsoleRegion::PAL, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("b9d9a0286c33dc6b7237bb13cd46fdee")},
  {"SCPH-100 (v4.3 03-11-00 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("8abc1b549a4a80954addc48ef02c4521")},
  {"SCPH-101 (v4.4 03-24-00 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("9a09ab7e49b422c007e6d54d7c49b965")},
  {"SCPH-101 (v4.5 05-25-00 A)", ConsoleRegion::NTSC_U, false, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("6e3735ff4c7dc899ee98981385f6f3d0")},
  {"SCPH-102 (v4.4 03-24-00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type1, 20, MakeHashFromString("b10f5e0e3d9eb60e5159690680b1e774")},
  {"SCPH-102 (v4.5 05-25-00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type1, 20, MakeHashFromString("de93caec13d1a141a40a79f5c86168d6")},
  {"SCPH-1000R (v4.5 05-25-00 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type1, 10, MakeHashFromString("476d68a94ccec3b9c8303bbd1daf2810")},
  {"PS2, SCPH-18000 (v5.0 10-27-00 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("d8f485717a5237285e4d7c5f881b7f32")},
  {"PS2, SCPH-30003 (v5.0 09-02-00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("71f50ef4f4e17c163c78908e16244f7d")},
  {"PS2, DTL-H10000 (v5.0 01/17/00 T)", ConsoleRegion::Auto, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("32f2e4d5ff5ee11072a6bc45530f5765")},
  {"PS2, SCPH-10000 (v5.0 01/17/00 T)", ConsoleRegion::Auto, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("acf4730ceb38ac9d8c7d8e21f2614600")},
  {"PS2, DTL-H10000 (v5.0 02/17/00 T)", ConsoleRegion::Auto, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("acf9968c8f596d2b15f42272082513d1")},
  {"PS2, SCPH-10000/SCPH-15000 (v5.0 02/17/00 T)", ConsoleRegion::Auto, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("b1459d7446c69e3e97e6ace3ae23dd1c")},
  {"PS2, DTL-H10000 (v5.0 02/24/00 T)", ConsoleRegion::Auto, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("d3f1853a16c2ec18f3cd1ae655213308")},
  {"PS2, DTL-H30001 (v5.0 07/27/00 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("63e6fd9b3c72e0d7b920e80cf76645cd")},
  {"PS2, SCPH-30001 (v5.0 07/27/00 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("a20c97c02210f16678ca3010127caf36")},
  {"PS2, SCPH-30001 (v5.0 09/02/00 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("8db2fbbac7413bf3e7154c1e0715e565")},
  {"PS2, DTL-H30002 (v5.0 09/02/00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("91c87cb2f2eb6ce529a2360f80ce2457")},
  {"PS2, DTL-H30102 (v5.0 09/02/00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("3016b3dd42148a67e2c048595ca4d7ce")},
  {"PS2, SCPH-30002/SCPH-30003/SCPH-30004 (v5.0 09/02/00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("b7fa11e87d51752a98b38e3e691cbf17")},
  {"PS2, SCPH-18000 (GH-003) (v5.0 10/27/00 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("f63bc530bd7ad7c026fcd6f7bd0d9525")},
  {"PS2, SCPH-18000 (GH-008) (v5.0 10/27/00 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("cee06bd68c333fc5768244eae77e4495")},
  {"PS2, DTL-H30101 (v5.0 12/28/00 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("0bf988e9c7aaa4c051805b0fa6eb3387")},
  {"PS2, SCPH-30001/SCPH-35001 (v5.0 12/28/00 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("8accc3c49ac45f5ae2c5db0adc854633")},
  {"PS2, DTL-H30102 (v5.0 12/28/00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("6f9a6feb749f0533aaae2cc45090b0ed")},
  {"PS2, SCPH-30002/SCPH-30003/SCPH-30004/SCHP-35002/SCPH-35003/SCPH-35004 (v5.0 12/28/00 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("838544f12de9b0abc90811279ee223c8")},
  {"PS2, DTL-H30000 (v5.0 01/18/01 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("bb6bbc850458fff08af30e969ffd0175")},
  {"PS2, SCPH-30000/SCPH-35000 (v5.0 01/18/01 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("815ac991d8bc3b364696bead3457de7d")},
  {"PS2, SCPH-30001R (v5.0 04/27/01 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("b107b5710042abe887c0f6175f6e94bb")},
  {"PS2, SCPH-30000 (v5.0 04/27/01 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("ab55cceea548303c22c72570cfd4dd71")},
  {"PS2, SCPH-30001R (v5.0 07/04/01 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("18bcaadb9ff74ed3add26cdf709fff2e")},
  {"PS2, SCPH-30002R/SCPH-30003R/SCPH-30004R (v5.0 07/04/01 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("491209dd815ceee9de02dbbc408c06d6")},
  {"PS2, SCPH-30001R (v5.0 10/04/01 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("7200a03d51cacc4c14fcdfdbc4898431")},
  {"PS2, SCPH-30002R/SCPH-30003R/SCPH-30004R (v5.0 10/04/01 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("8359638e857c8bc18c3c18ac17d9cc3c")},
  {"PS2, SCPH-30005R/SCPH-30006R/SCPH-30007R (v5.0 07/30/01 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("352d2ff9b3f68be7e6fa7e6dd8389346")},
  {"PS2, SCPH-39001 (v5.0 02/07/02 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("d5ce2c7d119f563ce04bc04dbc3a323e")},
  {"PS2, SCPH-39002/SCPH-39003/SCPH-39004 (v5.0 03/19/02 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("0d2228e6fd4fb639c9c39d077a9ec10c")},
  {"PS2, SCPH-37000/SCPH-39000 (v5.0 04/26/02 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("72da56fccb8fcd77bba16d1b6f479914")},
  {"PS2, SCPH-39008 (v5.0 04/26/02 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("5b1f47fbeb277c6be2fccdd6344ff2fd")},
  {"PS2, SCPH-39005/SCPH-39006/SCPH-39007 (v5.0 04/26/02 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("315a4003535dfda689752cb25f24785c")},
  {"PS2, DTL-H50000 (v5.0 02/06/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("54ecde087258557e2ddb5c3ddb004028")},
  {"PS2, SCPH-50000/SCPH-55000 (v5.0 02/06/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("312ad4816c232a9606e56f946bc0678a")},
  {"PS2, DTL-H50002 (v5.0 02/27/03 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("666018ffec65c5c7e04796081295c6c7")},
  {"PS2, SCPH-50002/SCPH-50003/SCPH-50004 (v5.0 02/27/03 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("6e69920fa6eef8522a1d688a11e41bc6")},
  {"PS2, DTL-H50001 (v5.0 03/25/03 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("eb960de68f0c0f7f9fa083e9f79d0360")},
  {"PS2, SCPH-50001 (v5.0 03/25/03 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("8aa12ce243210128c5074552d3b86251")},
  {"PS2, DTL-H50009 (v5.0 02/24/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("240d4c5ddd4b54069bdc4a3cd2faf99d")},
  {"PS2, DESR-5000/DESR-5100/DESR-7000/DESR-7100 (v5.0 10/28/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("1c6cd089e6c83da618fbf2a081eb4888")},
  {"PS2, SCPH-55000 (v5.0 06/23/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("463d87789c555a4a7604e97d7db545d1")},
  {"PS2, DTL-H50001 (v5.0 06/23/03 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("ab9d49ad40ae49f19856ad187777b1b3")},
  {"PS2, SCPH-50001/SCPH-50010 (v5.0 06/23/03 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("35461cecaa51712b300b2d6798825048")},
  {"PS2, SCPH-50002/SCPH-50003/SCPH-50004 (v5.0 06/23/03 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("bd6415094e1ce9e05daabe85de807666")},
  {"PS2, SCPH-50006/SCPH-50007 (v5.0 06/23/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("2e70ad008d4ec8549aada8002fdf42fb")},
  {"PS2, SCPH-50005 (v5.0 06/23/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("50d5b97b57d8c9b6534adcb46c2027d4")},
  {"PS2, SCPH-50008 (v5.0 06/23/03 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("b53d51edc7fc086685e31b811dc32aad")},
  {"PS2, SCPH-50009 (v5.0 06/23/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("1b6e631b536247756287b916f9396872")},
  {"PS2, SCPH-50000 (v5.0 08/22/03 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("00da1b177096cfd2532c8fa22b43e667")},
  {"PS2, SCPH-50004 (v5.0 08/22/03 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("afde410bd026c16be605a1ae4bd651fd")},
  {"PS2, SCPH-50011 (v5.0 03/29/04 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("81f4336c1de607dd0865011c0447052e")},
  {"PS2, SCPH-70000 (v5.0 06/14/04 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("0eee5d1c779aa50e94edd168b4ebf42e")},
  {"PS2, SCPH-70001/SCPH-70011/SCPH-70012 (v5.0 06/14/04 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("d333558cc14561c1fdc334c75d5f37b7")},
  {"PS2, SCPH-70002/SCPH-70003/SCPH-70004/SCPH-70008 (v5.0 06/14/04 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("dc752f160044f2ed5fc1f4964db2a095")},
  {"PS2, SCPH-70002 (v5.0 06/14/04 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("7ebb4fc5eab6f79a27d76ac9aad392b2")},
  {"PS2, DTL-H70002 (v5.0 06/14/04 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("63ead1d74893bf7f36880af81f68a82d")},
  {"PS2, SCPH-70005/SCPH-70006/SCPH-70007 (v5.0 06/14/04 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("3e3e030c0f600442fa05b94f87a1e238")},
  {"PS2, DESR-5500/DESR-5700/DESR-7500/DESR-7700 (v5.0 09/17/04 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("1ad977bb539fc9448a08ab276a836bbc")},
  {"PS2, DTL-H75000 (v5.0 06/20/05 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("bf0078ba5e19d57eae18047407f3b6e5")},
  {"PS2, SCPH-75000 (v5.0 06/20/05 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("eb4f40fcf4911ede39c1bbfe91e7a89a")},
  {"PS2, DTL-H75000A (v5.0 06/20/05 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("9959ad7a8685cad66206e7752ca23f8b")},
  {"PS2, SCPH-75001/SCPH-75010 (v5.0 06/20/05 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("929a14baca1776b00869f983aa6e14d2")},
  {"PS2, SCPH-75002/SCPH-75003/SCPH-75004/SCPH-75008 (v5.0 06/20/05 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("573f7d4a430c32b3cc0fd0c41e104bbd")},
  {"PS2, SCPH-75006 (v5.0 06/20/05 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("df63a604e8bff5b0599bd1a6c2721bd0")},
  {"PS2, SCPH-77000 (v5.0 02/10/06 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("5b1ba4bb914406fae75ab8e38901684d")},
  {"PS2, SCPH-77001/SCPH-77010 (v5.0 02/10/06 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("cb801b7920a7d536ba07b6534d2433ca")},
  {"PS2, SCPH-77002/SCPH-77003/SCPH-77004/SCPH-77008 (v5.0 02/10/06 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("af60e6d1a939019d55e5b330d24b1c25")},
  {"PS2, SCPH-77006/SCPH-77007 (v5.0 02/10/06 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("549a66d0c698635ca9fa3ab012da7129")},
  {"PS2, DTL-H90000 (v5.0 09/05/06 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("5e2014472c88f74f7547d8c2c60eca45")},
  {"PS2, SCPH-79000/SCPH-90000 (v5.0 09/05/06 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("5de9d0d730ff1e7ad122806335332524")},
  {"PS2, DTL-H90000 (v5.0 09/05/06 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("21fe4cad111f7dc0f9af29477057f88d")},
  {"PS2, SCPH-79001/SCPH-79010/SCPH-90001 (v5.0 09/05/06 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("40c11c063b3b9409aa5e4058e984e30c")},
  {"PS2, SCPH-79002/SCPH-79003/SCPH-79004/SCPH-79008/SCPH-90002/SCPH-90003/SCPH-90004 (v5.0 09/05/06 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("80bbb237a6af9c611df43b16b930b683")},
  {"PS2, SCPH-79006/SCPH-79007/SCPH-90006/SCPH-90007 (v5.0 09/05/06 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("c37bce95d32b2be480f87dd32704e664")},
  {"PS2, SCPH-90000 (v5.0 02/20/08 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100, MakeHashFromString("80ac46fa7e77b8ab4366e86948e54f83")},
  {"PS2, SCPH-90001/SCPH-90010 (v5.0 02/20/08 A)", ConsoleRegion::NTSC_U, true, ImageInfo::FastBootPatch::Type2, 100 , MakeHashFromString("21038400dc633070a78ad53090c53017")},
  {"PS2, SCPH-90002/SCPH-90003/SCPH-90004/SCPH-90008 (v5.0 02/20/08 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 100 , MakeHashFromString("dc69f0643a3030aaa4797501b483d6c4")},
  {"PS2, SCPH-90005/SCPH-90006/SCPH-90007 (v5.0 02/20/08 J)", ConsoleRegion::NTSC_J, true, ImageInfo::FastBootPatch::Type2, 100 , MakeHashFromString("30d56e79d89fbddf10938fa67fe3f34e")},
  {"PS2, KDL-22PX300 (v5.0 04/15/10 E)", ConsoleRegion::PAL, true, ImageInfo::FastBootPatch::Type2, 150, MakeHashFromString("93ea3bcee4252627919175ff1b16a1d9")},
};
// clang-format on

// OpenBIOS is separate, because there's no fixed hash for it. So just in case something collides with a hash of zero...
// which would be unlikely.
static constexpr const ImageInfo s_openbios_info = {
  "OpenBIOS", ConsoleRegion::Auto, false, ImageInfo::FastBootPatch::Unsupported, 200, {}};
static constexpr const char s_openbios_signature[] = {'O', 'p', 'e', 'n', 'B', 'I', 'O', 'S'};
static constexpr u32 s_openbios_signature_offset = 0x78;

} // namespace BIOS

bool BIOS::ImageInfo::CanSlowBootDisc(DiscRegion disc_region) const
{
  // BIOSes without region checks can slow boot anything, those with region checks can't slow boot mismatched games.
  if (!region_check)
    return true;

  // Boot to BIOS for non-PS1 discs, e.g. audio.
  if (disc_region == DiscRegion::NonPS1)
    return true;

  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      return (disc_region == DiscRegion::NTSC_J);
    case ConsoleRegion::NTSC_U:
      return (disc_region == DiscRegion::NTSC_U);
    case ConsoleRegion::PAL:
      return (disc_region == DiscRegion::PAL);
    default:
      return false;
  }
}

TinyString BIOS::ImageInfo::GetHashString(const BIOS::ImageInfo::Hash& hash)
{
  return TinyString::from_format(
    "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", hash[0],
    hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8], hash[9], hash[10], hash[11], hash[12],
    hash[13], hash[14], hash[15]);
}

std::optional<BIOS::Image> BIOS::LoadImageFromFile(const char* filename, Error* error)
{
  std::optional<BIOS::Image> ret;

  auto fp = FileSystem::OpenManagedCFile(filename, "rb", error);
  if (!fp)
  {
    Error::AddPrefixFmt(error, "Failed to open BIOS '{}': ", Path::GetFileName(filename));
    return ret;
  }

  const u64 size = static_cast<u64>(FileSystem::FSize64(fp.get()));
  if (size != BIOS_SIZE && size != BIOS_SIZE_PS2 && size != BIOS_SIZE_PS3)
  {
    Error::SetStringFmt(error, "BIOS image '{}' size mismatch, expecting either {} or {} bytes but got {} bytes",
                        Path::GetFileName(filename), static_cast<unsigned>(BIOS_SIZE),
                        static_cast<unsigned>(BIOS_SIZE_PS2), size);
    return ret;
  }

  // We want to hash the whole file. That means reading the whole thing in, if it's a larger BIOS (PS2).
  std::optional<DynamicHeapArray<u8>> data = FileSystem::ReadBinaryFile(fp.get(), error);
  if (!data.has_value() || data->size() < BIOS_SIZE)
    return ret;

  ret = BIOS::Image();
  ret->hash = MD5Digest::HashData(data.value());

  // But only copy the first 512KB, since that's all that's mapped.
  ret->data = std::move(data.value());
  ret->data.resize(BIOS_SIZE);
  ret->info = GetInfoForHash(ret->data, ret->hash);

  DEV_LOG("Hash for BIOS '{}': {}", FileSystem::GetDisplayNameFromPath(filename), ImageInfo::GetHashString(ret->hash));
  return ret;
}

const BIOS::ImageInfo* BIOS::GetInfoForHash(const std::span<const u8> image, const ImageInfo::Hash& hash)
{
  // check for openbios
  if (image.size() >= (s_openbios_signature_offset + std::size(s_openbios_signature)) &&
      std::memcmp(&image[s_openbios_signature_offset], s_openbios_signature, std::size(s_openbios_signature)) == 0)
  {
    return &s_openbios_info;
  }

  for (const ImageInfo& ii : s_image_info_by_hash)
  {
    if (ii.hash == hash)
      return &ii;
  }

  WARNING_LOG("Unknown BIOS hash: {}", ImageInfo::GetHashString(hash));
  return nullptr;
}

bool BIOS::IsValidBIOSForRegion(ConsoleRegion console_region, ConsoleRegion bios_region)
{
  return (console_region == ConsoleRegion::Auto || bios_region == ConsoleRegion::Auto || bios_region == console_region);
}

bool BIOS::PatchBIOSFastBoot(u8* image, u32 image_size, ImageInfo::FastBootPatch type)
{
  // Replace the shell entry point with a return back to the bootstrap.
  static constexpr const u32 shell_replacement[] = {
    // lui at, 1f80
    // lui t2, 0300h
    // sw t2, 1814h(at) ; turn the display on
    // jr ra
    // nop
    Mips::Encoder::lui(Mips::Encoder::Reg::AT, 0x1F80),
    Mips::Encoder::lui(Mips::Encoder::Reg::T2, 0x0300),
    Mips::Encoder::sw(Mips::Encoder::Reg::T2, 0x1814, Mips::Encoder::Reg::AT),
    Mips::Encoder::jr(Mips::Encoder::Reg::RA),
    Mips::Encoder::nop(),
  };

  // Type1/Type2 use the same shell replacement patch, but for historical reasons we replace the actual shell code for
  // Type 1, and the routine that calls the decompressor for Part 2.
  u32 patch_offset;
  if (type == ImageInfo::FastBootPatch::Type1)
  {
    // Try the "new" fast boot (Type 1B), where we patch the routine that copies/decompresses the shell.
    // This saves approximately 2 seconds of fast boot time, because the memcpy() executes out of uncached ROM...
    static constexpr const char* search_pattern = "e0 ff bd 27"  // add sp, sp, -20
                                                  "1c 00 bf af"  // sw ra, 0xc(sp)
                                                  "20 00 a4 af"  // sw a0, 0x20(sp)
                                                  "?? ?? 05 3c"  // lui a1, 0xbfc1
                                                  "?? ?? 06 3c"  // lui a2, 0x6
                                                  "?? ?? c6 34"  // ori a2, a2, 0x7ff0
                                                  "?? ?? a5 34"  // ori a1, a1, 0x8000
                                                  "?? ?? ?? 0f"; // jal 0xbfc02b50
    const std::optional<size_t> offset =
      StringUtil::BytePatternSearch(std::span<const u8>(image, image_size), search_pattern);
    if (offset.has_value())
    {
      patch_offset = static_cast<u32>(offset.value());
      VERBOSE_LOG("Found Type 1B pattern at offset 0x{:08X}", patch_offset);
    }
    else
    {
      patch_offset = 0x18000;
      INFO_LOG("Using Type 1A fast boot patch at offset 0x{:08X}.", patch_offset);
    }
  }
  else if (type == ImageInfo::FastBootPatch::Type2)
  {
    static constexpr const char* search_pattern = "d8 ff bd 27"  // add sp, sp, -28
                                                  "1c 00 bf af"  // sw ra, 0xc(sp)
                                                  "28 00 a4 af"  // sw a0, 0x28(sp)
                                                  "?? ?? 06 3c"  // lui a2, 0xbfc6
                                                  "?? ?? c6 24"  // addiu a2, -0x6bb8
                                                  "c0 bf 04 3c"  // lui a0, 0xbfc0
                                                  "?? ?? ?? 0f"; // jal 0xbfc58720
    constexpr u32 FALLBACK_OFFSET = 0x00052AFC;
    const std::optional<size_t> offset =
      StringUtil::BytePatternSearch(std::span<const u8>(image, image_size), search_pattern);
    if (offset.has_value())
    {
      patch_offset = static_cast<u32>(offset.value());
      VERBOSE_LOG("Found Type 2 pattern at offset 0x{:08X}", patch_offset);
    }
    else
    {
      patch_offset = FALLBACK_OFFSET;
      WARNING_LOG("Failed to find Type 2 pattern in BIOS image. Using fallback offset of 0x{:08X}", patch_offset);
    }
  }
  else [[unlikely]]
  {
    return false;
  }

  Assert((patch_offset + sizeof(shell_replacement)) <= image_size);
  std::memcpy(image + patch_offset, shell_replacement, sizeof(shell_replacement));
  return true;
}

bool BIOS::IsValidPSExeHeader(const PSEXEHeader& header, size_t file_size)
{
  static constexpr char expected_id[] = {'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  if (file_size < sizeof(expected_id) || std::memcmp(header.id, expected_id, sizeof(expected_id)) != 0)
    return false;

  if ((header.file_size + sizeof(PSEXEHeader)) > file_size)
  {
    WARNING_LOG("Incorrect file size in PS-EXE header: {} bytes should not be greater than {} bytes", header.file_size,
                file_size - sizeof(PSEXEHeader));
  }

  return true;
}

DiscRegion BIOS::GetPSExeDiscRegion(const PSEXEHeader& header)
{
  static constexpr char ntsc_u_id[] = "Sony Computer Entertainment Inc. for North America area";
  static constexpr char ntsc_j_id[] = "Sony Computer Entertainment Inc. for Japan area";
  static constexpr char pal_id[] = "Sony Computer Entertainment Inc. for Europe area";

  if (std::memcmp(header.marker, ntsc_u_id, sizeof(ntsc_u_id) - 1) == 0)
    return DiscRegion::NTSC_U;
  else if (std::memcmp(header.marker, ntsc_j_id, sizeof(ntsc_j_id) - 1) == 0)
    return DiscRegion::NTSC_J;
  else if (std::memcmp(header.marker, pal_id, sizeof(pal_id) - 1) == 0)
    return DiscRegion::PAL;
  else
    return DiscRegion::Other;
}

std::optional<BIOS::Image> BIOS::GetBIOSImage(ConsoleRegion region, Error* error)
{
  std::string bios_name;
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      bios_name = Core::GetStringSettingValue("BIOS", "PathNTSCJ", "");
      break;

    case ConsoleRegion::PAL:
      bios_name = Core::GetStringSettingValue("BIOS", "PathPAL", "");
      break;

    case ConsoleRegion::NTSC_U:
    default:
      bios_name = Core::GetStringSettingValue("BIOS", "PathNTSCU", "");
      break;
  }

  std::optional<Image> image;

  if (bios_name.empty())
  {
    // auto-detect
    image = FindBIOSImageInDirectory(region, EmuFolders::Bios.c_str(), error);
  }
  else
  {
    // try the configured path
    image = LoadImageFromFile(Path::Combine(EmuFolders::Bios, bios_name).c_str(), error);
  }

  // verify region
  if (image.has_value() && (!image->info || !IsValidBIOSForRegion(region, image->info->region)))
  {
    WARNING_LOG("BIOS region {} does not match requested region {}. This may cause issues.",
                image->info ? Settings::GetConsoleRegionName(image->info->region) : "UNKNOWN",
                Settings::GetConsoleRegionName(region));
  }

  return image;
}

std::optional<BIOS::Image> BIOS::FindBIOSImageInDirectory(ConsoleRegion region, const char* directory, Error* error)
{
  INFO_LOG("Searching for a {} BIOS in '{}'...", Settings::GetConsoleRegionName(region), directory);
  Error::Clear(error);

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(
    directory, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);

  std::optional<Image> image;
  std::string image_path;
  bool image_region_match = false;

  for (const FILESYSTEM_FIND_DATA& fd : results)
  {
    if (fd.Size != BIOS_SIZE && fd.Size != BIOS_SIZE_PS2 && fd.Size != BIOS_SIZE_PS3)
    {
      WARNING_LOG("Skipping '{}': incorrect size", fd.FileName.c_str());
      continue;
    }

    std::string full_path(Path::Combine(directory, fd.FileName));
    std::optional<Image> found_image = LoadImageFromFile(full_path.c_str(), error);
    if (!found_image.has_value())
      continue;

    // don't let an unknown bios take precedence over a known one
    const bool region_match = (found_image->info && IsValidBIOSForRegion(region, found_image->info->region));
    if (image.has_value() &&
        ((image->info && !found_image->info) || (image_region_match && !region_match) ||
         (image->info && found_image->info && image->info->priority < found_image->info->priority)))
    {
      continue;
    }

    image = std::move(found_image);
    image_path = std::move(full_path);
    image_region_match = region_match;
  }

  if (!image.has_value())
  {
    if (Error::IsValid(error))
      Error::AddSuffix(error, "\n\n");

#ifndef __ANDROID__
    Error::AddSuffixFmt(
      error,
      TRANSLATE_FS("System", "No BIOS image found for {} region.\n\nDuckStation requires a PS1 or PS2 BIOS in order to "
                             "run.\n\nFor legal reasons, you *must* obtain a BIOS from an actual PS1 unit that you own "
                             "(borrowing doesn't count).\n\nOnce dumped, this BIOS image should be placed in the bios "
                             "folder within the data directory (Tools Menu -> Open Data Directory)."),
      Settings::GetConsoleRegionName(region));
#else
    Error::AddSuffixFmt(error, TRANSLATE_FS("System", "No BIOS image found for {} region."),
                        Settings::GetConsoleRegionName(region));
#endif
    return image;
  }

  if (!image->info)
    WARNING_LOG("Using unknown BIOS '{}'. This may crash.", Path::GetFileName(image_path));

  return image;
}

std::vector<std::pair<std::string, const BIOS::ImageInfo*>> BIOS::FindBIOSImagesInDirectory(const char* directory)
{
  std::vector<std::pair<std::string, const ImageInfo*>> results;

  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(directory, "*",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    if (fd.Size != BIOS_SIZE && fd.Size != BIOS_SIZE_PS2 && fd.Size != BIOS_SIZE_PS3)
      continue;

    std::string full_path(Path::Combine(directory, fd.FileName));
    std::optional<Image> found_image = LoadImageFromFile(full_path.c_str(), nullptr);
    if (!found_image)
      continue;

    results.emplace_back(std::move(fd.FileName), found_image->info);
  }

  return results;
}

bool BIOS::HasAnyBIOSImages()
{
  return FindBIOSImageInDirectory(ConsoleRegion::Auto, EmuFolders::Bios.c_str(), nullptr).has_value();
}
