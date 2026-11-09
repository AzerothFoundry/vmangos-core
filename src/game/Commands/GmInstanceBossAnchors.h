#ifndef GM_INSTANCE_BOSS_ANCHORS_H
#define GM_INSTANCE_BOSS_ANCHORS_H
// Native source-derived fallback anchors. They never summon or alter an encounter.
// Full provenance: docs/GM_INSTANCE_BOSS_ANCHORS.json
struct GmInstanceBossAnchor { uint32 map,entry,parent; float x,y,z,o; };
static GmInstanceBossAnchor const GmInstanceBossAnchors[] = {
    // src/scripts/kalimdor/feralas/dire_maul/dreadsteed_ritual.cpp:588 native_summon
    {429,14506,0,-39.5763f,812.786f,-29.4525f,2.26893f},
    // src/scripts/eastern_kingdoms/eastern_plaguelands/stratholme/instance_stratholme.cpp:752 native_summon
    {329,10516,0,3719.82f,-3426.25f,131.844f,3.3412f},
    // src/scripts/eastern_kingdoms/eastern_plaguelands/stratholme/instance_stratholme.cpp:981 scripted_combat_home
    {329,10439,0,4033.009f,-3404.3293f,115.3554f,4.78897f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_spire/boss_halycon.cpp:77 native_summon
    {229,10268,0,-167.58f,-382.41f,64.401f,1.563f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_spire/instance_blackrock_spire.cpp:756 native_summon
    {229,10264,0,43.7685f,-259.82f,91.6483f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/molten_core/instance_molten_core.cpp:828 native_summon
    {409,12018,0,758.089f,-1176.71f,-118.64f,3.12414f},
    // src/scripts/eastern_kingdoms/burning_steppes/molten_core/boss_majordomo_executus.cpp:312 native_summon
    {409,11502,0,838.308f,-831.466f,-232.185f,2.19911f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1156 native_summon
    {230,9437,0,815.6f,-168.54f,-49.75f,5.97f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1161 native_summon
    {230,9438,0,846.66f,-317.18f,-50.29f,3.9f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1166 native_summon
    {230,9439,0,963.27f,-343.73f,-71.74f,2.22f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1171 native_summon
    {230,9441,0,545.49f,-162.49f,-35.46f,5.86f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1176 native_summon
    {230,9442,0,681.52f,-11.55f,-60.06f,1.98f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1181 native_summon
    {230,9443,0,803.64f,-248.0f,-43.3f,2.6f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1216 native_summon
    {230,9537,0,856.087f,-149.747f,-49.672f,0.059f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:1263 native_summon
    {230,9476,0,819.45f,-348.96f,-50.49f,0.35f},
    // src/scripts/eastern_kingdoms/western_plaguelands/scholomance/instance_scholomance.cpp:247 native_summon
    {289,1853,0,180.771f,-5.4286f,75.5702f,1.29154f},
    // src/scripts/eastern_kingdoms/western_plaguelands/scholomance/instance_scholomance.cpp:272 native_summon
    {289,10506,0,315.028f,70.53845f,102.1496f,0.3859715f},
    // src/scripts/eastern_kingdoms/eastern_plaguelands/naxxramas/naxxramas.h:305 native_summon
    {533,15989,0,3522.56f,-5236.76f,137.626f,4.4855f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackwing_lair/boss_nefarian.cpp:169 scripted_ground_arrival
    {469,11583,0,-7495.964f,-1252.402f,476.795f,0.0f},
    // src/scripts/kalimdor/silithus/temple_of_ahnqiraj/boss_ouro.cpp:450 parent_spawn
    {531,15517,15957,0.0f,0.0f,0.0f,0.0f},
    // src/scripts/eastern_kingdoms/eastern_plaguelands/stratholme/boss_dathrohan_balnazzar.cpp:226 parent_spawn
    {329,10813,10812,0.0f,0.0f,0.0f,0.0f},
    // src/scripts/eastern_kingdoms/eastern_plaguelands/stratholme/boss_timmy_the_cruel.cpp:107 native_summon
    {329,10808,0,3614.7f,-3187.64f,131.406f,0.0f},
    // src/scripts/kalimdor/the_barrens/wailing_caverns/wailing_caverns.cpp:503 native_summon
    {43,3654,0,142.7f,254.0f,-102.2f,0.0f},
    // src/scripts/eastern_kingdoms/dun_morogh/gnomeregan/gnomeregan.cpp:127 native_summon
    {90,7361,0,-476.3761f,-108.1901f,-145.7763f,1.919862f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,9027,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,9028,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,9029,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,9030,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,9031,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,9032,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // src/scripts/eastern_kingdoms/burning_steppes/blackrock_depths/blackrock_depths.cpp:191 scripted_combat_home
    {230,16059,0,596.285156f,-188.698944f,-54.132176f,0.0f},
    // mangos.event_scripts:0 native_summon
    {329,11121,0,3820.95f,-3695.15f,143.87f,1.825f},
    // mangos.event_scripts:0 native_summon
    {309,14515,0,-11540.7f,-1627.71f,41.27f,0.1f},
    // mangos.event_scripts:0 native_summon
    {209,7273,0,1664.65f,1187.67f,-2.88755f,0.812962f},
    // mangos.event_scripts:0 native_summon
    {329,11120,0,3584.41f,-2998.57f,125.0f,1.892f},
    // mangos.gameobject_scripts:0 native_summon
    {48,12876,0,-780.439f,-79.3731f,-44.0216f,0.680678f},
    // mangos.gameobject_scripts:0 native_summon
    {309,15083,0,-11901.3f,-1906.52f,65.36f,0.9f},
    // mangos.gameobject_scripts:0 native_summon
    {309,15084,0,-11901.3f,-1906.52f,65.36f,0.9f},
    // mangos.gameobject_scripts:0 native_summon
    {309,15085,0,-11901.3f,-1906.52f,65.36f,0.9f},
    // mangos.gameobject_scripts:0 native_summon
    {309,15082,0,-11901.3f,-1906.52f,65.36f,0.9f},
    // mangos.creature_ai_scripts:0 native_summon
    {189,14693,0,1798.01f,1223.17f,18.274f,4.71161f},
    // mangos.generic_scripts:0 native_summon
    {289,14516,0,135.97f,170.855f,95.7897f,2.98451f},
    // mangos.creature_ai_events / creature_ai_scripts / spell_template:0 parent_spawn
    {36,643,642,0.0f,0.0f,0.0f,0.0f},
};
#endif
