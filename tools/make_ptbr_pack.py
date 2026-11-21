#!/usr/bin/env python3
"""Builds assets/translations/pt-BR, the Brazilian Portuguese translation the
launcher offers as "Português (Brasil)".

The translation is the fan translation by XEX Brasil / Brazilian Warriors
(joao13traducoes.com, "Asura's Wrath - Tradução PT-BR V2.0"). On the console
it replaces the Spanish files, so the game runs in Spanish (user_language 5)
and these files take the place of the game's own (src/game/translation.cpp).

  make_ptbr_pack.py <patch folder> [<out folder>]

<patch folder> holds the patch's BCGame folder (as downloaded). <out folder>
defaults to the repository's assets/translations/pt-BR.

What it adds to the patch:
  - Coalesced_ESN.bin: quotes the game's fonts have (' in the subtitles); the lines the patch left in Spanish, or as the
    developers' placeholder names (the Spanish release has those too: the
    tutorial hints, Ryu's missions, a few menu messages), in Portuguese;
    and a few gallery texts that were half Spanish.
  - achievements.tsv (id, name, description): the achievements in
    Portuguese, from the game's own trophy list (Collection.esn PrizeMenu,
    which the patch translated); the Xbox achievement N is trophy N - 1.
  - CREDITS.txt: the translators, from the patch's LEIA-ME.
"""
import os
import shutil
import struct
import sys

# The translated images (title menu, Game Over, part and game end screens)
# are packages the patch stores uncompressed, so larger than the game's: the
# launcher gives the game a TOC with their sizes (FixGameToc,
# src/game/game_files.h), as the patch's own Xbox360TOC.txt does.
FILES = [
    "BCGame/Movies/00_AutoSave_Xenon_ESN.bik",
    "BCGame/CookedXbox360/BC-FrontEnd.xxx",
    "BCGame/CookedXbox360/GameOverMenu_SF.xxx",
    "BCGame/CookedXbox360/PartEnd_SF.xxx",
    "BCGame/CookedXbox360/PlayEnd_SF.xxx",
]
COALESCED = "BCGame/CookedXbox360/Coalesced_ESN.bin"
# The game's fonts have no curly quotes (the patch has one pair, shown as
# boxes). The subtitle font (TitleFont) has no " either, so the subtitles
# (aw_*.esn, where the Spanish release has a few " that box too) get ' instead.
STRAIGHT_QUOTES = str.maketrans({"\u201c": '"', "\u201d": '"', "\u2018": "'", "\u2019": "'"})
SUBTITLE_QUOTES = str.maketrans({"\u201c": "'", "\u201d": "'", '"': "'", "\u2018": "'", "\u2019": "'"})

# (localization file, section, key) -> text. Files by name, lower case,
# without the extension (BCGame\Localization\ESN\<name>.esn).
FIXES = {
    # Left in Spanish.
    ("aw_18", "msgId", "aw_18_3077"): "Então por que está se esquivando?",
    ("bcgame", "Synchronic", "SynchronicRankB"): "ÓTIMO",
    ("bcgame", "Advertise", "S100_Shoot"): "<Shoot> Espirrar",
    ("bcgame", "Mission", "Mission_OffsetCount"): "Hadokens cancelados:",
    ("collection", "PrizeMenu", "Prize37"): "A Coleção Completa",
    ("collection", "PrizeMenu", "Prize38"): "Ka-ching!",
    ("collection", "PrizeMenu", "PrizeExplan00"): "Desbloqueie todas as conquistas de ASURA'S WRATH.",
    ("collection", "WorldMenu", "WorldPicture01"): "Templo da Cidade-Fortaleza",
    ("collection", "WorldMenu", "WorldPicture52"): "Gohma Ferrão",
    ("collection", "WorldMenu", "WorldPicture64"): "O Septentrião",
    # Placeholders (the Spanish release shows the key itself).
    ("bcgame", "Advertise", "S000_ShootAimInfo"): "Use <LS> para mirar.",
    ("bcgame", "Advertise", "S000_Reaction"):
        "Insira o comando correto durante o \naviso na tela para contra-atacar.",
    ("bcgame", "Advertise", "S020_HeavyAssaultInfo"):
        "Use Ataques Pesados para eliminar\nos inimigos ao redor!",
    ("bcgame", "Mission", "Mission_20CountKO"): "K.O. em contagem de 20",
    ("bcgame", "Mission", "Mission_NoDamageKO"): "K.O. sem sofrer dano",
    ("bcgame", "Mission", "Mission_15CountKO"): "K.O. em contagem de 15",
    ("collection", "PrizeMenu", "Prize49"): "Pacificador de Planetas",
    ("collection", "PrizeMenu", "Prize50"): "Nirvana",
    ("collection", "PrizeMenu", "PrizeMenuHelp01"): "Ver detalhes do troféu.",
    ("collection", "PrizeMenu", "PrizeMenuHelp03"): "Ver detalhes da conquista.",
    ("menu", "ChapterMenu", "ChapterMenuHelp01"):
        "Escolha o episódio que deseja jogar. Você pode repetir episódios que já terminou.",
    ("option", "OptionMenu", "OptionDefaultMes01"): "Configurações padrão restauradas.",
    ("option", "OptionMenu", "OpSubtitlesMes00"):
        "As mensagens de ajuda não serão exibidas com as\nlegendas desligadas. Continuar?",
    # Gallery (Collection > World) texts that were half Spanish.
    ("collection", "WorldMenu", "WorldExplan26"):
        "Yasha mascarado, como aparece diante de Asura após seu retorno\n"
        "a Gaia. A máscara não tem outro propósito além de representar a\n"
        "supressão de seus sentimentos. Com ela, ele deixa as emoções de lado\n"
        "e faz o que for preciso pela causa de Deus.",
    ("collection", "WorldMenu", "WorldExplan28"):
        "Forma em que Deus se funde com a Fortaleza Karma\nna órbita de Gaia.\n"
        "Ainda mais colossal que a forma final de Wyzen, é o\núltimo recurso de Deus.\n"
        "Nesta forma final, ele pode manipular o próprio Brahmastra.",
    ("collection", "WorldMenu", "WorldExplan30"):
        "Uma das autoproclamadas Sete Divindades.\n"
        "Antes era um bruto que nunca pensava antes de agir, mas\n"
        "depois de ganhar novos poderes como um dos Sete, passou a se\n"
        "destacar pelo desprezo aos humanos.\n"
        "Faz questão de falar de forma condescendente.\n"
        "Sua afinidade de Mantra é a Violência.",
    ("collection", "WorldMenu", "WorldExplan32"):
        "Forma final de Wyzen. Usando o poder da Fortaleza Karma,\n"
        "Wyzen cresce até atingir o tamanho de um planeta. Porém,\n"
        "controlar o Mantra para manter esse tamanho exige um esforço\n"
        "enorme, o que deixa seus movimentos lentos.",
    ("collection", "WorldMenu", "WorldExplan33"):
        "Uma das autoproclamadas Sete Divindades.\n"
        "Um semideus que ama a beleza mais do que tudo. Apesar de seu ar\n"
        "artístico e de sua fala poética, toma as decisões mais cruéis\n"
        "sem a menor hesitação.\n"
        "Sua afinidade de Mantra é a Vaidade.",
    ("collection", "WorldMenu", "WorldExplan34"):
        "Uma das autoproclamadas Sete Divindades.\n"
        "Estrategista astuto e ardiloso, Kalrow faz os outros executarem\n"
        "seus planos e raramente se esforça para fazer algo ele mesmo.\n"
        "Pesquisa uma forma mais eficiente de controlar facilmente os\n"
        "humanos e os Gohma. Acha cansativo andar, por isso usa seu\n"
        "bastão com rodas para se locomover.\n"
        "Sua afinidade de Mantra é a Preguiça.",
    ("collection", "WorldMenu", "WorldExplan35"):
        "Uma das autoproclamadas Sete Divindades.\n"
        "Apesar de cego, é um dos guerreiros mais capazes dos Sete e um\n"
        "mestre espadachim. Ousado e generoso, mas implacável no campo\n"
        "de batalha. Mestre e mentor de Asura e Yasha, vê a luta não\n"
        "como um meio, mas como seu propósito de vida.\n"
        "Sua afinidade de Mantra é a Ganância.",
    ("collection", "WorldMenu", "WorldExplan38"):
        "A filha de Asura.\n"
        "Escolhida como a única Sacerdotisa de Shinkoku, é a única\n"
        "semideusa capaz de controlar e amplificar o Mantra em todo o\n"
        "seu poder.\n"
        "Querendo esse poder para si, Deus a sequestra durante o golpe\n"
        "de Estado.",
    ("collection", "WorldMenu", "WorldExplan40"):
        "O deus onipotente que controla o mundo de Gaia.\n"
        "Busca fazer nascer o redentor que tomará seu lugar (como o\n"
        "próximo deus) por meio das provações que impõe ao mundo,\n"
        "incluindo a criação do pior inimigo da civilização, o Gohma\n"
        "Vlitra. Para ele, o sofrimento do mundo é necessário para\n"
        "encontrar seu herdeiro.\n"
        "Ninguém em Gaia sabe de sua existência.",
    ("collection", "WorldMenu", "WorldExplan42"):
        "Soldados do tipo operador que gerenciam o sistema de comando\n"
        "do Exército de Shinkoku. Frios e sem emoções, esses soldados\n"
        "processam e transmitem informações em alta velocidade.",
    ("collection", "WorldMenu", "WorldExplan44"):
        "Soldados de alta patente do Exército de Shinkoku. Além de\n"
        "hábeis no combate corpo a corpo, seus corpos grandes permitem\n"
        "manejar armas pesadas com facilidade. Falam com honra e respeito\n"
        "às Sete Divindades, mas são rudes e condescendentes com os\n"
        "demais. Entre os Taison estão os Taison Nyudo, oponentes\n"
        "ainda mais temíveis.",
    ("collection", "WorldMenu", "WorldExplan45"):
        "Soldados de patente média do Exército de Shinkoku. Guerreiros\n"
        "ferozes especializados em combate direto, não mostram piedade\n"
        "com seus inimigos. Comandam as tropas de soldados Doji.",
    ("collection", "WorldMenu", "WorldExplan46"):
        "Soldados de baixa patente do Exército de Shinkoku. Não são\n"
        "muito competentes, mas sua força está no número e nas várias\n"
        "armas que empunham. Empolgam-se rápido e se confundem com\n"
        "facilidade. Costumam falar com frases curtas, de uma palavra.",
    ("collection", "WorldMenu", "WorldExplan77"):
        "Armas que assumem a forma de budas. Criadas por meio da\n"
        "conversão amplificada de Mantra, são imensamente poderosas e\n"
        "essenciais na guerra contra os Gohma.",
}


# --- Coalesced_<LANG>.bin: big-endian i32 counts, FStrings (positive length:
# Latin-1, negative: UTF-16LE; both with a terminating zero) ---

def parse(data):
    pos = 0

    def i32():
        nonlocal pos
        v = struct.unpack_from(">i", data, pos)[0]
        pos += 4
        return v

    def fstring():
        nonlocal pos
        n = i32()
        if n == 0:
            return None
        if n > 0:
            s = data[pos:pos + n].decode("latin-1")
            pos += n
        else:
            s = data[pos:pos - 2 * n].decode("utf-16-le")
            pos += -2 * n
        return s[:-1] if s.endswith("\0") else s

    files = []
    for _ in range(i32()):
        name = fstring()
        sections = []
        for _ in range(i32()):
            section = fstring()
            keys = [[fstring(), fstring()] for _ in range(i32())]
            sections.append([section, keys])
        files.append([name, sections])
    if pos != len(data):
        raise ValueError("Coalesced: trailing data")
    return files


def write(files):
    out = bytearray()

    def i32(v):
        out.extend(struct.pack(">i", v))

    def fstring(s):
        # The game's files have every string in UTF-16.
        if s is None:
            i32(0)
        else:
            b = (s + "\0").encode("utf-16-le")
            i32(-(len(b) // 2))
            out.extend(b)

    i32(len(files))
    for name, sections in files:
        fstring(name)
        i32(len(sections))
        for section, keys in sections:
            fstring(section)
            i32(len(keys))
            for k, v in keys:
                fstring(k)
                fstring(v)
    return bytes(out)


# The patch's edited images lost properties the game's own have (every one
# MipTailBaseIdx, some TextureFileCacheName): with MipTailBaseIdx 0 the game
# would describe the texture to the GPU as a packed mip tail from level 0.
# Each is put back
# as it is in the game's package (the bytes below), in place: the patch left
# that much free after the export. Its size in the export table and the file
# offsets of its bulk data grow by as much.
#   export table entry (SerialSize, SerialOffset), export offset, the patch's
#   size, where in the export the properties go, the properties
TEXTURE_REPAIRS = {
    "BCGame/CookedXbox360/BC-FrontEnd.xxx": [  # TitleMenu.TitleMenu_I24
        (0x3E20, 0xDE940, 65829, 159,
         "dc000000000000009a000000000000000800000000000000e3000000000000009200000000"
         "0000007c00000000000000040000000000000002000000"),
    ],
    "BCGame/CookedXbox360/GameOverMenu_SF.xxx": [  # GameOverMenu.ESN.GameOver_Text
        (0x91A, 0x254B3, 196941, 159, "1a000000000000001700000000000000040000000000000004000000"),
    ],
    "BCGame/CookedXbox360/PartEnd_SF.xxx": [  # PartEnd.PartEnd_I3
        (0x660, 0x3A9B, 262437, 159,
         "23000000000000000f00000000000000080000000000000028000000000000000e00000000"
         "0000000c00000000000000040000000000000005000000"),
    ],
    "BCGame/CookedXbox360/PlayEnd_SF.xxx": [  # PlayEnd.PlayEndBg
        (0x6FE, 0x7A19, 983423, 156, "00000011000000000000000e00000000000000040000000000000006"),
    ],
}


def repair_textures(path, repairs):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import asset_extract  # the package reader (tools/asset_extract.py)

    data = bytearray(open(path, "rb").read())
    for table_entry, offset, size, insert_at, props in repairs:
        props = bytes.fromhex(props)
        current, at = struct.unpack_from(">ii", data, table_entry)
        if (current, at) == (size + len(props), offset):
            continue  # already repaired
        if (current, at) != (size, offset) or any(data[offset + size:offset + size + len(props)]):
            sys.exit(f"{path}: not the patch file this tool knows (export at {offset:#x})")
        package = asset_extract.Package(path)
        blob = bytearray(data[offset:offset + size])
        _, pos = asset_extract.read_properties(package, bytes(blob), 4)
        if not 4 < insert_at <= pos:
            sys.exit(f"{path}: the export at {offset:#x} isn't as expected")
        # After the properties: the source art's bulk data, the mip count, each
        # mip's bulk data and its width and height. A bulk data header is
        # flags, element count, size, file offset (big-endian), and inline
        # data follows it: every offset that points at its own data moves.
        def bulk(pos):
            flags, _, n, file_offset = struct.unpack_from(">iiii", blob, pos)
            if file_offset == offset + pos + 16:
                struct.pack_into(">i", blob, pos + 12, file_offset + len(props))
            return pos + 16 + (n if n > 0 and not flags & 0x21 else 0)

        pos = bulk(pos)
        (mips,) = struct.unpack_from(">i", blob, pos)
        pos += 4
        for _ in range(mips):
            pos = bulk(pos) + 8
        blob[insert_at:insert_at] = props
        data[offset:offset + len(blob)] = blob
        struct.pack_into(">i", data, table_entry, len(blob))
    open(path, "wb").write(bytes(data))
    for table_entry, offset, _, _, _ in repairs:
        package = asset_extract.Package(path)
        export = next(e for e in package.exports if e["offset"] == offset)
        properties, _ = asset_extract.read_properties(package, package.blob(export), 4)
        if "MipTailBaseIdx" not in properties:
            sys.exit(f"{path}: repairing the export at {offset:#x} failed")


def strings_file(name):
    """bcgame for ..\\..\\BCGame\\Localization\\ESN\\BCGame.esn (not _DefaultFont)."""
    parts = name.replace("/", "\\").split("\\")
    if len(parts) < 3 or parts[-3].lower() != "localization" or parts[-2].upper() != "ESN":
        return None
    return parts[-1].rsplit(".", 1)[0].lower()


def main():
    args = sys.argv[1:]
    if len(args) not in (1, 2):
        sys.exit(__doc__)
    patch = args[0]
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = args[1] if len(args) == 2 else os.path.join(repo, "assets", "translations", "pt-BR")
    pack_files = FILES
    for f in pack_files + [COALESCED]:
        if not os.path.isfile(os.path.join(patch, f)):
            sys.exit(f"{patch}: no {f} (the folder that holds the patch's BCGame)")

    for f in pack_files:
        os.makedirs(os.path.dirname(os.path.join(out, f)), exist_ok=True)
        shutil.copyfile(os.path.join(patch, f), os.path.join(out, f))
    for f, repairs in TEXTURE_REPAIRS.items():
        if f in pack_files:
            repair_textures(os.path.join(out, f), repairs)

    files = parse(open(os.path.join(patch, COALESCED), "rb").read())
    pending = dict(FIXES)
    prizes = {}
    for name, sections in files:
        loc = strings_file(name)
        if not loc:
            continue
        for section, keys in sections:
            for kv in keys:
                fix = pending.pop((loc, section, kv[0]), None)
                if fix is not None:
                    kv[1] = fix
                if kv[1]:
                    kv[1] = kv[1].translate(SUBTITLE_QUOTES if loc.startswith("aw_") else STRAIGHT_QUOTES)
                if loc == "collection" and section == "PrizeMenu":
                    prizes[kv[0]] = kv[1]
    if pending:
        sys.exit(f"not in the patch's Coalesced_ESN.bin: {sorted(pending)}")
    os.makedirs(os.path.dirname(os.path.join(out, COALESCED)), exist_ok=True)
    open(os.path.join(out, COALESCED), "wb").write(write(files))

    # The achievements: Xbox achievement N (1..45) is trophy N - 1.
    def one_line(s):
        return " ".join(s.replace("\t", " ").split())

    rows = ["id\tname\tdescription"]
    for n in range(1, 46):
        key = f"{n - 1:02d}"
        rows.append(f"{n}\t{one_line(prizes['Prize' + key])}\t{one_line(prizes['PrizeExplan' + key])}")
    open(os.path.join(out, "achievements.tsv"), "w", encoding="utf-8", newline="\n").write(
        "\n".join(rows) + "\n")

    readme = [f for f in os.listdir(patch) if f.lower().startswith("leia-me")]
    original = ""
    if readme:
        original = open(os.path.join(patch, readme[0]), "rb").read().decode("cp1252").replace("\r\n", "\n")
    open(os.path.join(out, "CREDITS.txt"), "w", encoding="utf-8", newline="\n").write(
        "Asura's Wrath - Tradução PT-BR (V2.0, revisão 2025)\n"
        "Tradução: Brazilian Warriors (Xujozer, Angel Forgotten, Sergio-F)\n"
        "Adaptação e revisão: XEX BRASIL / JMC360\n"
        "Divulgação: https://joao13traducoes.com/2021/07/x360-asura-wrath-xex-brasil-e-brazilian-warriors/\n"
        "\n"
        "Used by Asura's Wrath Recompiled as the launcher's \"Português (Brasil)\"\n"
        "language, with the lines the patch left in Spanish or untranslated\n"
        "completed (tools/make_ptbr_pack.py).\n"
        "\n--- LEIA-ME original ---\n\n" + original)
    print(f"{out}: {len(pack_files) + 1} game files, achievements.tsv, CREDITS.txt ({len(FIXES)} lines fixed)")


if __name__ == "__main__":
    main()
