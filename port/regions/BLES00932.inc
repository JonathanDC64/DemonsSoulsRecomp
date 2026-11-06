/* BLES00932, the European disc (01.00): its address for every BLUS30443 address the port
 * uses (des_region.h). Sorted by the US address; one line per address.
 * tools/region/region_table.py checks that every address in the port is here (CI), and
 * finds and checks the European address from both decrypted EBOOTs. */
DES_REGION_ADDR(0x000112E8, 0x000112E8)  /* hook des_body_sample */
DES_REGION_ADDR(0x00023750, 0x00024338)  /* call */
DES_REGION_ADDR(0x000B14C8, 0x000B22B0)  /* call kSpacingPlay */
DES_REGION_ADDR(0x000B15C8, 0x000B23B0)  /* call kFontPlay */
DES_REGION_ADDR(0x000D5B24, 0x000D6B0C)  /* hook des_cam_own */
DES_REGION_ADDR(0x00110608, 0x001115D0)  /* hook des_mouse_look */
DES_REGION_ADDR(0x001107C0, 0x00111788)  /* hook des_cam_step */
DES_REGION_ADDR(0x00119670, 0x0011A638)  /* hook des_mouse_look */
DES_REGION_ADDR(0x00119818, 0x0011A7E0)  /* hook des_cam_rate */
DES_REGION_ADDR(0x001198D8, 0x0011A8A0)  /* hook des_cam_rate */
DES_REGION_ADDR(0x00119FAC, 0x0011AF74)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011A018, 0x0011AFE0)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011A100, 0x0011B0C8)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011B830, 0x0011C7F8)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011C0E8, 0x0011D0B0)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011C15C, 0x0011D124)  /* hook des_cam_keep */
DES_REGION_ADDR(0x0011C2BC, 0x0011D284)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011C758, 0x0011D720)  /* hook des_mouse_look */
DES_REGION_ADDR(0x0011C910, 0x0011D8D8)  /* hook des_mouse_look */
DES_REGION_ADDR(0x0011CF84, 0x0011DF4C)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011CF98, 0x0011DF60)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011D2D4, 0x0011E29C)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011D6E4, 0x0011E6AC)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0011D820, 0x0011E7E8)  /* hook des_cam_rate */
DES_REGION_ADDR(0x00123100, 0x001240C8)  /* hook des_cam_rate */
DES_REGION_ADDR(0x00123164, 0x0012412C)  /* hook des_cam_rate */
DES_REGION_ADDR(0x00124188, 0x00125150)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0012554C, 0x00126514)  /* hook des_cam_rate */
DES_REGION_ADDR(0x001256D4, 0x0012669C)  /* hook des_cam_rate */
DES_REGION_ADDR(0x0012EE08, 0x0012FDD0)  /* call, wrapped (des_aspect) */
DES_REGION_ADDR(0x0012F074, 0x0013003C)  /* hook des_cam_ctor */
DES_REGION_ADDR(0x0012F0DC, 0x001300A4)  /* hook des_cam_ctor */
DES_REGION_ADDR(0x0012F19C, 0x00130164)  /* hook des_frustum_tall */
DES_REGION_ADDR(0x0015AE10, 0x0015BDD8)  /* call kLoadSouls */
DES_REGION_ADDR(0x001D17A0, 0x001D2628)  /* call */
DES_REGION_ADDR(0x001D1E64, 0x001D2CEC)  /* hook des_phys_rate */
DES_REGION_ADDR(0x001D35D8, 0x001D4460)  /* call */
DES_REGION_ADDR(0x001DF83C, 0x001E06C4)  /* hook des_phys_kf */
DES_REGION_ADDR(0x001EF6D0, 0x001F0558)  /* hook des_phys_kf */
DES_REGION_ADDR(0x001FEAF4, 0x001FF97C)  /* hook des_slide_rate */
DES_REGION_ADDR(0x001FED50, 0x001FFBD8)  /* hook des_slide_rate */
DES_REGION_ADDR(0x00217E70, 0x00218CF8)  /* hook des_motion_blur */
DES_REGION_ADDR(0x002184DC, 0x00219364)  /* hook des_dof */
DES_REGION_ADDR(0x00226778, 0x00227608)  /* call */
DES_REGION_ADDR(0x0025EB88, 0x0025FA30)  /* hook des_fade_rate */
DES_REGION_ADDR(0x0025EC14, 0x0025FABC)  /* hook des_fade_rate */
DES_REGION_ADDR(0x0026558C, 0x00266434)  /* hook des_fade_rate */
DES_REGION_ADDR(0x00265C88, 0x00266B30)  /* hook des_fade_rate */
DES_REGION_ADDR(0x0026FB20, 0x002709C8)  /* hook des_dev_hook */
DES_REGION_ADDR(0x0026FC48, 0x00270AF0)  /* hook des_dev_hook */
DES_REGION_ADDR(0x0026FC98, 0x00270B40)  /* hook des_dev_hook */
DES_REGION_ADDR(0x00273620, 0x002744C8)  /* call */
DES_REGION_ADDR(0x002737A0, 0x00274648)  /* call */
DES_REGION_ADDR(0x00273BE0, 0x00274A88)  /* call */
DES_REGION_ADDR(0x00273D38, 0x00274BE0)  /* hook des_move_post */
DES_REGION_ADDR(0x00273D4C, 0x00274BF4)  /* return address in 0x00273D38 */
DES_REGION_ADDR(0x0027882C, 0x002796D4)  /* hook des_dev_hook */
DES_REGION_ADDR(0x002802E0, 0x00281188)  /* call */
DES_REGION_ADDR(0x00280724, 0x002815CC)  /* hook des_dev_hook */
DES_REGION_ADDR(0x00280C78, 0x00281B20)  /* hook des_dev_hook */
DES_REGION_ADDR(0x00280E64, 0x00281D0C)  /* hook des_dev_hook */
DES_REGION_ADDR(0x00280F88, 0x00281E30)  /* hook des_dev_hook */
DES_REGION_ADDR(0x002818D0, 0x00282778)  /* hook des_dev_hook */
DES_REGION_ADDR(0x00286614, 0x002873EC)  /* hook des_dev_hook */
DES_REGION_ADDR(0x0028B224, 0x0028BFFC)  /* hook des_dev_hook */
DES_REGION_ADDR(0x002986B8, 0x00299488)  /* call */
DES_REGION_ADDR(0x002A6AA8, 0x002A7880)  /* wrapped kChrInsThread */
DES_REGION_ADDR(0x002A6AF4, 0x002A78CC)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B14, 0x002A78EC)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B24, 0x002A78FC)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B34, 0x002A790C)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B50, 0x002A7928)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B60, 0x002A7938)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B7C, 0x002A7954)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6B98, 0x002A7970)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6BA0, 0x002A7978)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6BB0, 0x002A7988)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6BD0, 0x002A79A8)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6BEC, 0x002A79C4)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6BF8, 0x002A79D0)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6C34, 0x002A7A0C)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6C58, 0x002A7A30)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6C60, 0x002A7A38)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6C70, 0x002A7A48)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002A6CA8, 0x002A7A80)  /* return address in 0x002A6AA8 */
DES_REGION_ADDR(0x002D8978, 0x002D9750)  /* call */
DES_REGION_ADDR(0x002DD368, 0x002DE140)  /* call kUseGoods */
DES_REGION_ADDR(0x002EC2B8, 0x002ED090)  /* call */
DES_REGION_ADDR(0x002EDEB0, 0x002EEC88)  /* hook des_dev_hook */
DES_REGION_ADDR(0x002F5B14, 0x002F68EC)  /* hook des_ik_rate */
DES_REGION_ADDR(0x002F5CCC, 0x002F6AA4)  /* hook des_ik_rate */
DES_REGION_ADDR(0x002F5DB8, 0x002F6B90)  /* hook des_ik_rate */
DES_REGION_ADDR(0x002FF8F0, 0x003006C8)  /* hook des_roll_dir */
DES_REGION_ADDR(0x00301C24, 0x003029FC)  /* hook des_roll_dir */
DES_REGION_ADDR(0x0030C330, 0x0030D108)  /* hook des_net_sync */
DES_REGION_ADDR(0x0030C378, 0x0030D150)  /* hook des_net_sync */
DES_REGION_ADDR(0x0030C46C, 0x0030D244)  /* hook des_net_sync */
DES_REGION_ADDR(0x0030F2BC, 0x00310094)  /* hook des_roll_tie */
DES_REGION_ADDR(0x0030F734, 0x0031050C)  /* hook des_roll_dir */
DES_REGION_ADDR(0x00313B24, 0x003148FC)  /* hook des_net_sync */
DES_REGION_ADDR(0x003147EC, 0x003155C4)  /* hook des_ghost_rate */
DES_REGION_ADDR(0x00314ABC, 0x00315894)  /* hook des_ghost_rate */
DES_REGION_ADDR(0x00315030, 0x00315E08)  /* hook des_ghost_rate */
DES_REGION_ADDR(0x00391818, 0x003925E0)  /* hook des_net_replay */
DES_REGION_ADDR(0x003957B8, 0x00396580)  /* hook des_net_replay */
DES_REGION_ADDR(0x003A596C, 0x003A6734)  /* hook des_net_replay */
DES_REGION_ADDR(0x003A5CA8, 0x003A6A70)  /* hook des_net_show */
DES_REGION_ADDR(0x003A5EA0, 0x003A6C68)  /* hook des_net_replay */
DES_REGION_ADDR(0x003D42BC, 0x003D509C)  /* hook des_twist_rate */
DES_REGION_ADDR(0x003E2DF0, 0x003E3BD0)  /* hook des_sun_gate */
DES_REGION_ADDR(0x003E6330, 0x003E7110)  /* hook des_sun_gate */
DES_REGION_ADDR(0x003E6518, 0x003E72F8)  /* hook des_sun_gate */
DES_REGION_ADDR(0x004418B8, 0x004426A0)  /* call */
DES_REGION_ADDR(0x0054E8C0, 0x0054F6B0)  /* hook des_net_show */
DES_REGION_ADDR(0x0054E8F8, 0x0054F6E8)  /* hook des_net_show */
DES_REGION_ADDR(0x0054E934, 0x0054F724)  /* hook des_net_grade_poll */
DES_REGION_ADDR(0x00558884, 0x00559674)  /* hook des_net_show */
DES_REGION_ADDR(0x00560904, 0x005616F4)  /* hook des_net_show */
DES_REGION_ADDR(0x00562BC4, 0x005639B4)  /* hook des_net_show */
DES_REGION_ADDR(0x00567E9C, 0x00568C8C)  /* hook des_qwc_local */
DES_REGION_ADDR(0x00595B80, 0x00596970)  /* call */
DES_REGION_ADDR(0x00597108, 0x00597EF8)  /* call */
DES_REGION_ADDR(0x00597388, 0x00598178)  /* call */
DES_REGION_ADDR(0x0059E3A8, 0x0059F198)  /* call */
DES_REGION_ADDR(0x005A5920, 0x005A6710)  /* call */
DES_REGION_ADDR(0x005C2810, 0x005C3600)  /* call */
DES_REGION_ADDR(0x005C2848, 0x005C3638)  /* call */
DES_REGION_ADDR(0x005F7254, 0x005F7E9C)  /* hook des_cut_cam */
DES_REGION_ADDR(0x005F72EC, 0x005F7F34)  /* hook des_cut_cam */
DES_REGION_ADDR(0x005F7394, 0x005F7FDC)  /* hook des_cut_cam */
DES_REGION_ADDR(0x005F743C, 0x005F8084)  /* hook des_cut_cam */
DES_REGION_ADDR(0x006A2E8C, 0x006A3A9C)  /* hook des_noise_rate */
DES_REGION_ADDR(0x006A3990, 0x006A45A0)  /* hook des_noise_rate */
DES_REGION_ADDR(0x00718D84, 0x00719994)  /* hook des_hud_canvas */
DES_REGION_ADDR(0x0071A6B8, 0x0071B2C8)  /* call kDlgBaseUpdate */
DES_REGION_ADDR(0x0071B5A0, 0x0071C1B0)  /* call kFontSpacing */
DES_REGION_ADDR(0x0071B728, 0x0071C338)  /* call kFontBegin */
DES_REGION_ADDR(0x00720F50, 0x00721B60)  /* call */
DES_REGION_ADDR(0x00720F80, 0x00721B90)  /* call */
DES_REGION_ADDR(0x007212A8, 0x00721EB8)  /* call */
DES_REGION_ADDR(0x00721490, 0x007220A0)  /* call */
DES_REGION_ADDR(0x007216D0, 0x007222E0)  /* call */
DES_REGION_ADDR(0x007216F0, 0x00722300)  /* call */
DES_REGION_ADDR(0x00723450, 0x00723F90)  /* call */
DES_REGION_ADDR(0x00723498, 0x00723FD8)  /* call */
DES_REGION_ADDR(0x00724AD0, 0x00725620)  /* call */
DES_REGION_ADDR(0x00724E40, 0x00725990)  /* hook des_menu_tick */
DES_REGION_ADDR(0x0072D970, 0x0072F1B8)  /* call */
DES_REGION_ADDR(0x007731F0, 0x00774A78)  /* call */
DES_REGION_ADDR(0x00789BC0, 0x0078B448)  /* call kGaugeUpdate */
DES_REGION_ADDR(0x0078E2E8, 0x0078FB70)  /* call kSmoothGaugeUpdate */
DES_REGION_ADDR(0x0078EB60, 0x007903E8)  /* call kGaugeDraw */
DES_REGION_ADDR(0x0078EDC8, 0x00790650)  /* call kGaugeDraw */
DES_REGION_ADDR(0x0078F020, 0x007908A8)  /* call kGaugeDraw */
DES_REGION_ADDR(0x00798CD8, 0x0079A560)  /* call kScrollMsgUpdate */
DES_REGION_ADDR(0x0079EF7C, 0x007A0804)  /* hook des_hud_w2s */
DES_REGION_ADDR(0x007A6264, 0x007A7AEC)  /* hook des_hud_onscreen */
DES_REGION_ADDR(0x007A8C60, 0x007AA4E8)  /* call kGetSoulUpdate */
DES_REGION_ADDR(0x007B5AE8, 0x007B7370)  /* hook des_hud_onscreen */
DES_REGION_ADDR(0x007B61D4, 0x007B7A5C)  /* hook des_hud_onscreen */
DES_REGION_ADDR(0x007DC7A0, 0x007DE028)  /* call */
DES_REGION_ADDR(0x007E4248, 0x007E5AD0)  /* call */
DES_REGION_ADDR(0x0081E110, 0x0081F9E0)  /* call kCmdChoice */
DES_REGION_ADDR(0x008219E0, 0x008232B0)  /* call kInvMessage */
DES_REGION_ADDR(0x008AEB68, 0x008B0440)  /* hook des_hud_onscreen */
DES_REGION_ADDR(0x008CD078, 0x008CE950)  /* call kNowLoadingUpdate */
DES_REGION_ADDR(0x008DA0E0, 0x008DB9B8)  /* hook des_viewer_rate */
DES_REGION_ADDR(0x008DB170, 0x008DCA48)  /* hook des_viewer_rate */
DES_REGION_ADDR(0x008EF430, 0x008F0DE0)  /* call kLogoUpdate */
DES_REGION_ADDR(0x008F2A58, 0x008F44D0)  /* call kBackPlateUpdate */
DES_REGION_ADDR(0x008FB4F0, 0x008FCF68)  /* call */
DES_REGION_ADDR(0x008FD2A8, 0x008FED20)  /* call */
DES_REGION_ADDR(0x0090FCD0, 0x00911748)  /* call */
DES_REGION_ADDR(0x009E0148, 0x009E1BC0)  /* call */
DES_REGION_ADDR(0x00A8CA48, 0x00A8E4C0)  /* call */
DES_REGION_ADDR(0x00A8CBF0, 0x00A8E668)  /* call */
DES_REGION_ADDR(0x00A9DD58, 0x00A9F7D0)  /* hook des_game_error */
DES_REGION_ADDR(0x00AC8078, 0x00AC9AF0)  /* boot patch (apply_boot_patches.py) */
DES_REGION_ADDR(0x00B495B8, 0x00B4B030)  /* call */
DES_REGION_ADDR(0x00B5CFA0, 0x00B5EA18)  /* call */
DES_REGION_ADDR(0x00B5D2F8, 0x00B5ED70)  /* call */
DES_REGION_ADDR(0x00B5D548, 0x00B5EFC0)  /* call */
DES_REGION_ADDR(0x00CD4854, 0x00CD62CC)  /* hook des_hud_ctx */
DES_REGION_ADDR(0x00D229A4, 0x00D2441C)  /* hook des_viewport_room */
DES_REGION_ADDR(0x00F60BD0, 0x00F62648)  /* call kDialogDraw */
DES_REGION_ADDR(0x00F62E68, 0x00F648E0)  /* call */
DES_REGION_ADDR(0x00FF54D4, 0x00FF6F4C)  /* hook des_ik_rate */
DES_REGION_ADDR(0x00FF5680, 0x00FF70F8)  /* hook des_ik_rate */
DES_REGION_ADDR(0x00FF58CC, 0x00FF7344)  /* hook des_ik_rate */
DES_REGION_ADDR(0x0109F2C4, 0x010A0D3C)  /* hook des_body_xform */
DES_REGION_ADDR(0x012D86C4, 0x012DA13C)  /* hook des_rag_pose_a */
DES_REGION_ADDR(0x012D9354, 0x012DADCC)  /* hook des_rag_pose_b */
DES_REGION_ADDR(0x013B75A8, 0x013B9020)  /* hook des_fx_state */
DES_REGION_ADDR(0x013B75D4, 0x013B904C)  /* hook des_fx_state */
DES_REGION_ADDR(0x013B7940, 0x013B93B8)  /* hook des_fx_state */
DES_REGION_ADDR(0x013B7B9C, 0x013B9614)  /* hook des_fx_state */
DES_REGION_ADDR(0x013B7CA4, 0x013B971C)  /* hook des_fx_state */
DES_REGION_ADDR(0x013B7CCC, 0x013B9744)  /* hook des_fx_state */
DES_REGION_ADDR(0x0167AC58, 0x0167C6D0)  /* hook ds_free */
DES_REGION_ADDR(0x0167AC88, 0x0167C700)  /* hook ds_malloc */
DES_REGION_ADDR(0x016EBB98, 0x016EDC68)  /* data kNoName */
DES_REGION_ADDR(0x0185AF98, 0x0185AF98)  /* vtable */
DES_REGION_ADDR(0x01867F50, 0x01867F98)  /* data kCursorVt */
DES_REGION_ADDR(0x018698A0, 0x018698E8)  /* data kHudDialogVt */
DES_REGION_ADDR(0x0186A968, 0x0186A9B0)  /* data kTitleVt */
DES_REGION_ADDR(0x0186E1E0, 0x0186E228)  /* data kFadePlateVt */
DES_REGION_ADDR(0x0186FBC0, 0x0186FC08)  /* data kLoadingVt */
DES_REGION_ADDR(0x01870C30, 0x01870C78)  /* data kBackPlateVt */
DES_REGION_ADDR(0x018B4810, 0x018B4858)  /* vtable */
DES_REGION_ADDR(0x018B4F98, 0x018B4FE0)  /* vtable */
DES_REGION_ADDR(0x018C8DB4, 0x018C8F20)  /* data kFrameDt */
DES_REGION_ADDR(0x018CEFB4, 0x018CF134)  /* data kCameraFov */
DES_REGION_ADDR(0x018CEFBC, 0x018CF13C)  /* data kCameraAspect */
DES_REGION_ADDR(0x0190433C, 0x01904494)  /* data kContextAspect */
DES_REGION_ADDR(0x019B5040, 0x019B5270)  /* data kToc, the TOC base (r2) */
DES_REGION_ADDR(0x01AD6298, 0x01AD6518)  /* data kFlipModeObj */
DES_REGION_ADDR(0x01B3A28C, 0x01B3A50C)  /* data kRemoMan */
DES_REGION_ADDR(0x01B3D6E0, 0x01B3D960)  /* data kChrMan */
DES_REGION_ADDR(0x01B4B150, 0x01B4B3D0)  /* data kMenuSeMgr */
DES_REGION_ADDR(0x01B4EF9C, 0x01B4F21C)  /* data kPgdRoot */
