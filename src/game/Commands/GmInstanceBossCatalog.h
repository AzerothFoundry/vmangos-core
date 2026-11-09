#ifndef GM_INSTANCE_BOSS_CATALOG_H
#define GM_INSTANCE_BOSS_CATALOG_H
// Boss/NPC ID facts derived from AtlasLootClassic vanilla data (not its code/assets),
// commit 8e99341e4e779328460bf7684c0d5b22ce50ddf1, with native quest-boss supplements.
// Each entry/name verified against this server's active build5875 creature templates.
// World-only NPCs, service NPCs and unnamed trash excluded. Group encounters list members.
// Positions are resolved from native current-map spawn data at runtime, never invented here.
struct GmInstanceBossEntry { uint32 map; uint32 entry; };
static GmInstanceBossEntry const GmInstanceBossCatalog[] = {
    {33, 3914}, // Rethilgore
    {33, 3886}, // Razorclaw the Butcher
    {33, 3887}, // Baron Silverlaine
    {33, 4278}, // Commander Springvale
    {33, 4279}, // Odo the Blindwatcher
    {33, 3872}, // Deathsworn Captain
    {33, 4274}, // Fenrus the Devourer
    {33, 3927}, // Wolf Master Nandos
    {33, 4275}, // Archmage Arugal
    {33, 14682}, // Sever
    {34, 1666}, // Kam Deepfury
    {34, 1720}, // Bruegal Ironknuckle
    {34, 1696}, // Targorr the Dread
    {34, 1663}, // Dextren Ward
    {34, 1717}, // Hamhock
    {34, 1716}, // Bazil Thredd
    {36, 644}, // Rhahk'Zor
    {36, 3586}, // Miner Johnson
    {36, 643}, // Sneed
    {36, 642}, // Sneed's Shredder
    {36, 1763}, // Gilnid
    {36, 646}, // Mr. Smite
    {36, 647}, // Captain Greenskin
    {36, 639}, // Edwin VanCleef
    {36, 645}, // Cookie
    {43, 3669}, // Lord Cobrahn
    {43, 3671}, // Lady Anacondra
    {43, 3653}, // Kresh
    {43, 3670}, // Lord Pythas
    {43, 3674}, // Skum
    {43, 3673}, // Lord Serpentis
    {43, 5775}, // Verdan the Everliving
    {43, 3654}, // Mutanus the Devourer
    {43, 5912}, // Deviate Faerie Dragon
    {47, 4424}, // Aggem Thorncurse
    {47, 4428}, // Death Speaker Jargba
    {47, 4420}, // Overlord Ramtusk
    {47, 4422}, // Agathelos the Raging
    {47, 4425}, // Blind Hunter
    {47, 4421}, // Charlga Razorflank
    {47, 4842}, // Earthcaller Halmgar
    {47, 6168}, // Roogug
    {48, 4887}, // Ghamoo-ra
    {48, 4831}, // Lady Sarevess
    {48, 6243}, // Gelihast
    {48, 12876}, // Baron Aquanis
    {48, 4832}, // Twilight Lord Kelris
    {48, 4830}, // Old Serra'kis
    {48, 4829}, // Aku'mai
    {48, 12902}, // Lorgus Jett
    {70, 6907}, // Eric "The Swift"
    {70, 6906}, // Baelog
    {70, 6908}, // Olaf
    {70, 6910}, // Revelosh
    {70, 7228}, // Ironaya
    {70, 7023}, // Obsidian Sentinel
    {70, 7206}, // Ancient Stone Keeper
    {70, 7291}, // Galgann Firehammer
    {70, 4854}, // Grimlok
    {70, 2748}, // Archaedas
    {90, 7361}, // Grubbis
    {90, 7079}, // Viscous Fallout
    {90, 6235}, // Electrocutioner 6000
    {90, 6229}, // Crowd Pummeler 9-60
    {90, 6228}, // Dark Iron Ambassador
    {90, 7800}, // Mekgineer Thermaplugg
    {109, 5716}, // Zul'Lor
    {109, 5712}, // Zolo
    {109, 5717}, // Mijan
    {109, 5714}, // Loro
    {109, 5715}, // Hukku
    {109, 5713}, // Gasher
    {109, 8580}, // Atal'alarion
    {109, 5708}, // Spawn of Hakkar
    {109, 8443}, // Avatar of Hakkar
    {109, 5710}, // Jammal'an the Prophet
    {109, 5711}, // Ogom the Wretched
    {109, 5721}, // Dreamscythe
    {109, 5720}, // Weaver
    {109, 5722}, // Hazzas
    {109, 5719}, // Morphaz
    {109, 5709}, // Shade of Eranikus
    {129, 7355}, // Tuten'kash
    {129, 7357}, // Mordresh Fire Eye
    {129, 8567}, // Glutton
    {129, 7354}, // Ragglesnout
    {129, 7358}, // Amnennar the Coldbringer
    {129, 7356}, // Plaguemaw the Rotting
    {129, 14686}, // Lady Falther'ess
    {189, 3983}, // Interrogator Vishas
    {189, 6490}, // Azshir the Sleepless
    {189, 6488}, // Fallen Champion
    {189, 6489}, // Ironspine
    {189, 4543}, // Bloodmage Thalnos
    {189, 14693}, // Scorn
    {189, 3974}, // Houndmaster Loksey
    {189, 6487}, // Arcanist Doan
    {189, 3975}, // Herod
    {189, 4542}, // High Inquisitor Fairbanks
    {189, 3976}, // Scarlet Commander Mograine
    {189, 3977}, // High Inquisitor Whitemane
    {209, 8127}, // Antu'sul
    {209, 7272}, // Theka the Martyr
    {209, 10080}, // Sandarr Dunereaver
    {209, 7271}, // Witch Doctor Zum'rah
    {209, 7796}, // Nekrum Gutchewer
    {209, 7275}, // Shadowpriest Sezz'ziz
    {209, 10081}, // Dustwraith
    {209, 7274}, // Sandfury Executioner
    {209, 7604}, // Sergeant Bly
    {209, 7795}, // Hydromancer Velratha
    {209, 7273}, // Gahz'rilla
    {209, 7267}, // Chief Ukorz Sandscalp
    {209, 10082}, // Zerillis
    {229, 10263}, // Burning Felguard
    {229, 9219}, // Spirestone Butcher
    {229, 9196}, // Highlord Omokk
    {229, 9218}, // Spirestone Battle Lord
    {229, 9217}, // Spirestone Lord Magus
    {229, 9236}, // Shadow Hunter Vosh'gajin
    {229, 9237}, // War Master Voone
    {229, 9596}, // Bannok Grimaxe
    {229, 10596}, // Mother Smolderweb
    {229, 10376}, // Crystal Fang
    {229, 10584}, // Urok Doomhowl
    {229, 9736}, // Quartermaster Zigris
    {229, 10220}, // Halycon
    {229, 10268}, // Gizrul the Slavener
    {229, 9718}, // Ghok Bashguud
    {229, 9568}, // Overlord Wyrmthalak
    {229, 16080}, // Mor Grayhoof
    {229, 9816}, // Pyroguard Emberseer
    {229, 10264}, // Solakar Flamewreath
    {229, 10509}, // Jed Runewatcher
    {229, 10899}, // Goraluk Anvilcrack
    {229, 10339}, // Gyth
    {229, 10429}, // Warchief Rend Blackhand
    {229, 10430}, // The Beast
    {229, 10363}, // General Drakkisath
    {229, 16042}, // Lord Valthalak
    {230, 9025}, // Lord Roccor
    {230, 9018}, // High Interrogator Gerstahn
    {230, 9319}, // Houndmaster Grebmar
    {230, 9027}, // Gorosh the Dervish
    {230, 9028}, // Grizzle
    {230, 9029}, // Eviscerator
    {230, 9030}, // Ok'thor the Breaker
    {230, 9031}, // Anub'shiah
    {230, 9032}, // Hedrum the Creeper
    {230, 9024}, // Pyromancer Loregrain
    {230, 9438}, // Dark Keeper Bethek
    {230, 9442}, // Dark Keeper Ofgut
    {230, 9443}, // Dark Keeper Pelver
    {230, 9439}, // Dark Keeper Uggel
    {230, 9437}, // Dark Keeper Vorfalk
    {230, 9441}, // Dark Keeper Zimrel
    {230, 9041}, // Warder Stilgiss
    {230, 9042}, // Verek
    {230, 9476}, // Watchman Doomgrip
    {230, 9056}, // Fineous Darkvire
    {230, 9017}, // Lord Incendius
    {230, 9016}, // Bael'Gar
    {230, 9033}, // General Angerforge
    {230, 8983}, // Golem Lord Argelmach
    {230, 9537}, // Hurley Blackbreath
    {230, 9499}, // Plugger Spazzring
    {230, 9502}, // Phalanx
    {230, 9156}, // Ambassador Flamelash
    {230, 8923}, // Panzor the Invincible
    {230, 9034}, // Hate'rel
    {230, 9035}, // Anger'rel
    {230, 9036}, // Vile'rel
    {230, 9037}, // Gloom'rel
    {230, 9038}, // Seeth'rel
    {230, 9039}, // Doom'rel
    {230, 9040}, // Dope'rel
    {230, 9938}, // Magmus
    {230, 8929}, // Princess Moira Bronzebeard
    {230, 9019}, // Emperor Dagran Thaurissan
    {230, 16059}, // Theldren
    {249, 10184}, // Onyxia
    {289, 14861}, // Blood Steward of Kirtonos
    {289, 10506}, // Kirtonos the Herald
    {289, 10503}, // Jandice Barov
    {289, 11622}, // Rattlegore
    {289, 14516}, // Death Knight Darkreaver
    {289, 10433}, // Marduk Blackpool
    {289, 10432}, // Vectus
    {289, 10508}, // Ras Frostwhisper
    {289, 10505}, // Instructor Malicia
    {289, 11261}, // Doctor Theolen Krastinov
    {289, 10901}, // Lorekeeper Polkelt
    {289, 10507}, // The Ravenian
    {289, 10504}, // Lord Alexei Barov
    {289, 10502}, // Lady Illucia Barov
    {289, 1853}, // Darkmaster Gandling
    {289, 14695}, // Lord Blackwood
    {289, 16118}, // Kormok
    {309, 14517}, // High Priestess Jeklik
    {309, 14507}, // High Priest Venoxis
    {309, 14510}, // High Priestess Mar'li
    {309, 11382}, // Bloodlord Mandokir
    {309, 15082}, // Gri'lek
    {309, 15083}, // Hazza'rah
    {309, 15084}, // Renataki
    {309, 15085}, // Wushoolay
    {309, 15114}, // Gahz'ranka
    {309, 14509}, // High Priest Thekal
    {309, 14515}, // High Priestess Arlokk
    {309, 11380}, // Jin'do the Hexxer
    {309, 14834}, // Hakkar
    {329, 10393}, // Skul
    {329, 10558}, // Hearthsinger Forresten
    {329, 10516}, // The Unforgiven
    {329, 11143}, // Postmaster Malown
    {329, 10808}, // Timmy the Cruel
    {329, 11032}, // Malor the Zealous
    {329, 11120}, // Crimson Hammersmith
    {329, 10997}, // Cannon Master Willey
    {329, 10811}, // Archivist Galford
    {329, 10812}, // Grand Crusader Dathrohan
    {329, 10813}, // Balnazzar
    {329, 10435}, // Magistrate Barthilas
    {329, 10809}, // Stonespine
    {329, 10436}, // Baroness Anastari
    {329, 11121}, // Black Guard Swordsmith
    {329, 10437}, // Nerub'enkan
    {329, 10438}, // Maleki the Pallid
    {329, 10439}, // Ramstein the Gorger
    {329, 10440}, // Baron Rivendare
    {329, 14684}, // Balzaphon
    {349, 13738}, // Veng
    {349, 13282}, // Noxxion
    {349, 12258}, // Razorlash
    {349, 13739}, // Maraudos
    {349, 12236}, // Lord Vyletongue
    {349, 12237}, // Meshlok the Harvester
    {349, 12225}, // Celebras the Cursed
    {349, 12203}, // Landslide
    {349, 13601}, // Tinkerer Gizlock
    {349, 13596}, // Rotgrip
    {349, 12201}, // Princess Theradras
    {349, 13742}, // Kolk
    {349, 13741}, // Gelk
    {349, 13740}, // Magra
    {389, 11520}, // Taragaman the Hungerer
    {389, 11518}, // Jergosh the Invoker
    {389, 11517}, // Oggleflint
    {389, 11519}, // Bazzalan
    {409, 12118}, // Lucifron
    {409, 11982}, // Magmadar
    {409, 12259}, // Gehennas
    {409, 12057}, // Garr
    {409, 12264}, // Shazzrah
    {409, 12056}, // Baron Geddon
    {409, 11988}, // Golemagg the Incinerator
    {409, 12098}, // Sulfuron Harbinger
    {409, 12018}, // Majordomo Executus
    {409, 11502}, // Ragnaros
    {429, 14354}, // Pusillin
    {429, 11490}, // Zevrim Thornhoof
    {429, 13280}, // Hydrospawn
    {429, 14327}, // Lethtendris
    {429, 11492}, // Alzzin the Wildshaper
    {429, 16097}, // Isalien
    {429, 11489}, // Tendris Warpwood
    {429, 11488}, // Illyanna Ravenoak
    {429, 11487}, // Magister Kalendris
    {429, 11467}, // Tsu'zee
    {429, 11496}, // Immol'thar
    {429, 11486}, // Prince Tortheldrin
    {429, 14690}, // Revanchion
    {429, 14506}, // Lord Hel'nurath
    {429, 14326}, // Guard Mol'dar
    {429, 14322}, // Stomper Kreeg
    {429, 14321}, // Guard Fengus
    {429, 14323}, // Guard Slip'kik
    {429, 14325}, // Captain Kromcrush
    {429, 14324}, // Cho'Rush the Observer
    {429, 11501}, // King Gordok
    {469, 12435}, // Razorgore the Untamed
    {469, 13020}, // Vaelastrasz the Corrupt
    {469, 12017}, // Broodlord Lashlayer
    {469, 11983}, // Firemaw
    {469, 14601}, // Ebonroc
    {469, 11981}, // Flamegor
    {469, 14020}, // Chromaggus
    {469, 11583}, // Nefarian
    {509, 15348}, // Kurinnaxx
    {509, 15341}, // General Rajaxx
    {509, 15340}, // Moam
    {509, 15370}, // Buru the Gorger
    {509, 15369}, // Ayamiss the Hunter
    {509, 15339}, // Ossirian the Unscarred
    {531, 15263}, // The Prophet Skeram
    {531, 15543}, // Princess Yauj
    {531, 15544}, // Vem
    {531, 15511}, // Lord Kri
    {531, 15516}, // Battleguard Sartura
    {531, 15510}, // Fankriss the Unyielding
    {531, 15299}, // Viscidus
    {531, 15509}, // Princess Huhuran
    {531, 15275}, // Emperor Vek'nilash
    {531, 15276}, // Emperor Vek'lor
    {531, 15517}, // Ouro
    {531, 15727}, // C'Thun
    {533, 15956}, // Anub'Rekhan
    {533, 15953}, // Grand Widow Faerlina
    {533, 15952}, // Maexxna
    {533, 15954}, // Noth the Plaguebringer
    {533, 15936}, // Heigan the Unclean
    {533, 16011}, // Loatheb
    {533, 16061}, // Instructor Razuvious
    {533, 16060}, // Gothik the Harvester
    {533, 16064}, // Thane Korth'azz
    {533, 16065}, // Lady Blaumeux
    {533, 16062}, // Highlord Mograine
    {533, 16063}, // Sir Zeliek
    {533, 16028}, // Patchwerk
    {533, 15931}, // Grobbulus
    {533, 15932}, // Gluth
    {533, 15928}, // Thaddius
    {533, 15989}, // Sapphiron
    {533, 15990}, // Kel'Thuzad
    {230, 9543}, // Ribbly Screwspigot (native quest/miniboss)
};
#endif
