// Dashboard constants & static config — extracted from app.js (pure data).
export const NOISY_PACKETS = new Set(['MOVE', 'NEWTICK', 'PING', 'PONG', 'UPDATEACK', 'GOTOACK']);
export const MAX_ROWS = 2000;
export const MAX_PLUGIN_LOGS = 200;

  // RotMG class objectType → name (decimal = hex type in data/objects.xml, e.g. 0x0321 → 801 Necromancer)
export const CLASS_NAMES = {
    768: 'Rogue', // 0x0300
    775: 'Archer', // 0x0307
    782: 'Wizard', // 0x030e
    784: 'Priest', // 0x0310
    785: 'Samurai', // 0x0311
    796: 'Bard', // 0x031c
    797: 'Warrior', // 0x031d
    798: 'Knight', // 0x031e
    799: 'Paladin', // 0x031f
    800: 'Assassin', // 0x0320
    801: 'Necromancer', // 0x0321
    802: 'Huntress', // 0x0322
    803: 'Mystic', // 0x0323
    804: 'Trickster', // 0x0324
    805: 'Sorcerer', // 0x0325
    806: 'Ninja', // 0x0326
    817: 'Summoner', // 0x0331
    818: 'Kensei', // 0x0332
  };

  // Class sprite colors: [primary, secondary/dark, hair/hat]
export const CLASS_COLORS = {
    768: ['#6b5b4b','#463b3b','#2a2020'], // Rogue
    775: ['#4a8c3f','#2d5a1e','#1a3a12'], // Archer
    782: ['#3366dd','#2244aa','#1a2a66'], // Wizard
    784: ['#e0d870','#c0b040','#806820'], // Priest
    785: ['#aa3333','#882222','#551111'], // Samurai
    796: ['#aa8844','#886633','#553311'], // Bard
    797: ['#cc4433','#aa3322','#661a11'], // Warrior
    798: ['#8888aa','#666688','#444466'], // Knight
    799: ['#ccaa33','#aa8822','#665511'], // Paladin
    800: ['#7744aa','#553388','#331a55'], // Assassin
    801: ['#553366','#332244','#1a1133'], // Necromancer
    802: ['#3d7a2e','#2d5a1e','#1a3a12'], // Huntress
    803: ['#aa66aa','#884488','#552255'], // Mystic
    804: ['#aa5588','#884466','#553344'], // Trickster
    805: ['#6688cc','#4466aa','#223355'], // Sorcerer
    806: ['#333333','#1a1a1a','#0a0a0a'], // Ninja
    817: ['#33aaaa','#228888','#115555'], // Summoner
    818: ['#aa3344','#882233','#551122'], // Kensei
  };

export const SKIN_COLOR = '#F5CFA0';
export const SKIN_SHADOW = '#D4A870';
export const EAM_ASSETS = window.EAMAssets || {};
export const EAM_ITEMS = EAM_ASSETS.items || {};
export const EAM_ENCHANTMENTS = window.EAMEnchantments || EAM_ASSETS.enchantments || {};
export const ITEM_RARITY_ICONS = {
    1: 'enchantments/uncommon.png',
    2: 'enchantments/rare.png',
    3: 'enchantments/legendary.png',
    4: 'enchantments/divine.png',
  };
export const THEMES = [
    { id: 'dark', label: 'Dark' },
    { id: 'light', label: 'Light' },
    { id: 'sage', label: 'Sage' },
    { id: 'mist', label: 'Mist' },
    { id: 'forest', label: 'Forest' },
    { id: 'ocean', label: 'Ocean' },
    { id: 'ember', label: 'Ember' },
  ];

export const LANGUAGES = [
    { id: 'en', label: 'English' },
    { id: 'es', label: 'Español' },
    { id: 'de', label: 'Deutsch' },
    { id: 'pt', label: 'Português' },
    { id: 'ja', label: '日本語' },
  ];


  // 8x8 pixel sprite template for front-facing character
  // .: transparent, S: skin, s: skin shadow, P: primary, D: secondary/dark, H: hair/hat
export const SPRITE_TEMPLATE = [
    '..HHHH..',
    '.HSSSSH.',
    '.HSSSSH.',
    'DPPPPPD.',
    '.DPPPD..',
    '..PPPP..',
    '..D..D..',
    '..DD.DD.',
  ];
