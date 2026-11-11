"""Answer a few exact question forms from the current Java Wiki passages."""

import re


_NAME = r"[a-z][a-z0-9' -]{0,63}?"
_ARTICLE = r"(?:an? |one |the )?"
_COUNT = re.compile(
    rf"how many (?P<ingredient>{_NAME}) (?:make|craft|go into|"
    rf"are (?:in|(?:used|needed|required) (?:in|to (?:make|craft)))|"
    rf"do (?:i|we|you) (?:need|use) (?:for|to (?:make|craft))|"
    rf"does it take to (?:make|craft)) "
    rf"{_ARTICLE}(?P<item>{_NAME})(?: recipe)?"
)
_RECIPE_TAKES = re.compile(
    rf"how many (?P<ingredient>{_NAME}) does {_ARTICLE}(?P<item>{_NAME}) recipe (?:take|use)"
)
_RECIPE = re.compile(r"(?:^|; )Crafting recipe: ([^;]+)")
_CRAFTED = re.compile(r"^Name: ([^;]+); ")
_INGREDIENT = re.compile(r"([1-9][0-9]{0,2}) × ([A-Za-z][A-Za-z0-9' -]{0,63})")
_FIXED_COUNTS = " (fixed ingredient counts; other slots vary)"

_RECIPE_LIST = re.compile(
    rf"what (?:(?:ingredients|materials) (?:are (?:needed|required)|"
    rf"do (?:i|we|you) need)|do (?:i|we|you) need) to craft {_ARTICLE}(?P<item>{_NAME})"
)
_RECIPE_ROW = re.compile(
    r"(?:Name: [^;]+; )?Ingredients: (?P<ingredients>[^;]+); Crafting recipe: (?P<counts>[^;]+)"
    r"(?:; Notes: (?P<notes>[^;]+))?"
)
_EDITION_NOTE = re.compile(r"\b(?:Bedrock|Preview|Experimental)\b|\[\d+\]", re.I)
_OVERVIEW_FIELDS = (
    (
        re.compile(
            rf"what is the (?:maximum )?durability of (?:an? )?(?:new )?(?P<item>{_NAME})"
            rf"|how much durability does (?:an? )?(?:new )?(?P<tool>{_NAME}) have"
        ),
        "Durability",
        r"[1-9][0-9]*",
    ),
    (
        re.compile(rf"what is the (?:max|maximum) (?:enchantment )?level (?:of|for) (?P<item>{_NAME})"),
        "Maximum level",
        r"[IVX]+(?: \([1-9][0-9]*\))?",
    ),
    (
        re.compile(
            rf"(?:is|are) {_ARTICLE}(?P<item>{_NAME}) stackable"
            rf"|what is the (?:maximum )?stack size of {_ARTICLE}(?P<stack>{_NAME})"
        ),
        "Stackable",
        r"(?:No|Yes \([1-9][0-9]*\)[^;]*)",
    ),
    (
        re.compile(
            rf"(?:is|are) {_ARTICLE}(?P<item>{_NAME}) waterloggable"
            rf"|can {_ARTICLE}(?P<block>{_NAME}) be waterlogged"
        ),
        "Waterloggable",
        r"Yes|No",
    ),
    (
        re.compile(rf"what is the (?:block )?hardness of {_ARTICLE}(?P<item>{_NAME})"),
        "Hardness",
        r"-?[0-9]+(?:\.[0-9]+)?",
    ),
    (
        re.compile(rf"what is the blast resistance of {_ARTICLE}(?P<item>{_NAME})"),
        "Blast resistance",
        r"[0-9]+(?:,[0-9]{3})*(?:\.[0-9]+)?",
    ),
)
_SLOTS = re.compile(
    rf"how many (?:inventory |storage )?slots (?:"
    rf"does {_ARTICLE}(?P<item>{_NAME}) have|are (?:in|inside) {_ARTICLE}(?P<container>{_NAME}))"
)
_SLOTS_VALUE = re.compile(
    r"\b(?:has|have|contains?|can be used as a container and has) "
    r"(?P<value>[1-9][0-9]{0,3}) (?:inventory |storage )?slots?"
    r"(?: of inventory space| for storage)?(?=[,.]| and |$)"
)
_FUEL_ROW = re.compile(
    r"Fuel: [^;]+; Burning time \(seconds\): [0-9.]+(?: [0-9]+:[0-9]{2})? [0-9]+ ticks; "
    r"Number of operations per fuel: (?P<amount>[1-9][0-9]{0,2}); "
    r"Number of operations per stack \(64\) of fuel: [0-9.]+(?: \([0-9.]+\))?; "
    r"Seconds per log: (?:None used|[0-9 .⁄()~]+); "
    r"Seconds per bamboo: (?:None used|[0-9 .⁄()~]+); "
    r"Amount needed to smelt a stack \(64\) of items: [0-9 .⁄()~]+"
)

_SIGNAL = re.compile(
    rf"what (?:redstone )?(?:signal strength|strength|power level) (?:is|does) "
    rf"{_ARTICLE}(?P<item>{_NAME})(?:'s)? (?:output|emit|give off)"
)
_LIGHT = re.compile(
    rf"what light level does {_ARTICLE}(?:normal )?(?P<item>{_NAME}) (?:emit|give off)"
)
_PULSE = re.compile(
    rf"how long is (?:the |a )?pulse (?:from|of) {_ARTICLE}(?P<item>{_NAME}) in game ticks"
)
_FUEL = re.compile(
    rf"how many items can (?:a single|one|an?) (?:piece of )?(?:regular )?"
    rf"(?P<item>{_NAME}) smelt(?: in a (?:regular )?furnace)?"
)
_SIGNAL_VALUE = re.compile(
    r"\b(?:emits?|outputs?|produces?)\b[^.;\n]{0,90}\bsignal strength "
    r"(?:of )?(?P<value>[0-9]|1[0-5])\b"
)
_LIGHT_VALUE = re.compile(
    r"\b(?:gives? off|emits?) (?:a )?light level of (?P<value>[0-9]|1[0-5])\b"
)
_PULSE_VALUE = re.compile(
    r"\b(?:emits?|outputs?|produces?)\b[^.;\n]{0,90}\bpulse\b[^.;\n]{0,90}"
    r"\bfor (?P<value>[1-9][0-9]{0,3}) game ticks\b"
)


def _singular(phrase):
    if phrase.endswith("shelves"):
        return phrase.removesuffix("shelves") + "shelf"
    # Keep names such as glass, prismarine bricks, and moss distinct.
    if phrase.endswith("s") and not phrase.endswith(("ss", "us", "is", " bricks")):
        return phrase[:-1]
    return phrase


def _recipe_count(item, ingredient, passages):
    hits = {}
    for number, passage in enumerate(passages, 1):
        if passage["title"].casefold() != item or passage["section"] != "Crafting":
            continue
        text = passage["text"]
        name = _CRAFTED.match(text)
        if name and name[1].casefold() != item:
            continue
        recipe_text, _, notes = text.partition("; Notes: ")
        recipe = _RECIPE.search(recipe_text.removesuffix(_FIXED_COUNTS))
        if not recipe or _EDITION_NOTE.search(text):
            # An applicable recipe without explicit counts may disagree.
            return None
        entries = [_INGREDIENT.fullmatch(entry) for entry in recipe[1].split(", ")]
        if not all(entries):
            return None
        matches = [
            (entry[1], entry[2].casefold())
            for entry in entries
            if _singular(entry[2].casefold()) == ingredient
        ]
        if len(matches) != 1:
            return None
        amount, material = matches[0]
        answer = f"Recipe uses {amount} × {material}."
        if notes:
            answer += " " + notes
        if len(answer) > 360:
            return None
        hits.setdefault(answer, number)
    if len(hits) != 1:
        return None
    answer, number = next(iter(hits.items()))
    return f"{answer} [{number}]"


def _recipe_list(item, passages):
    hits = {}
    for number, passage in enumerate(passages, 1):
        if passage["title"].casefold() != item or passage["section"] != "Crafting":
            continue
        text = passage["text"]
        if not text.startswith(("Name: ", "Ingredients: ")):
            continue
        name = _CRAFTED.match(text)
        if name and name[1].casefold() != item:
            continue
        row = _RECIPE_ROW.fullmatch(text)
        if not row or _EDITION_NOTE.search(text):
            return None
        entries = [_INGREDIENT.fullmatch(entry) for entry in row["counts"].split(", ")]
        if not all(entries) or sum(int(entry[1]) for entry in entries) > 9:
            return None
        if set(row["ingredients"].casefold().split(" + ")) != {entry[2].casefold() for entry in entries}:
            return None
        answer = f"Recipe ingredients: {row['counts']}."
        if row["notes"]:
            answer += " " + row["notes"]
        if len(answer) > 360:
            return None
        hits.setdefault(answer, number)
    if len(hits) != 1:
        return None
    answer, number = next(iter(hits.items()))
    return f"{answer} [{number}]"


def _overview_field(item, label, value_pattern, passages, *, plural):
    hits = {}
    prefix = label + ": "
    for number, passage in enumerate(passages, 1):
        title = passage["title"].casefold()
        if passage["section"] != "Overview":
            continue
        if title != item and not (plural and (_singular(item) == title or item == title + "es")):
            continue
        line = passage["text"]
        if not line.startswith(prefix):
            continue
        value = line.removeprefix(prefix)
        if len(line) > 200 or _EDITION_NOTE.search(value) or not re.fullmatch(value_pattern, value):
            return None
        hits.setdefault((title, line), number)
    if len(hits) != 1:
        return None
    (_, line), number = next(iter(hits.items()))
    return f"{line} [{number}]"


def _property(item, pattern, passages, sections=("Behavior", "Light", "Overview"), *, allow_leading_condition=True):
    hits = {}
    start = r"(?:^|, )(?:then )?" if allow_leading_condition else "^"
    subject = re.compile(
        rf"{start}(?:a |an |the |all )?{re.escape(item)}(?:s|es)? (?:also |normally |always )?$"
    )
    for number, passage in enumerate(passages, 1):
        if passage["title"].casefold() != item or passage["section"] not in sections:
            continue
        for sentence in re.split(r"(?<=[.!?])\s+|\n", passage["text"]):
            if len(sentence) > 220 or _EDITION_NOTE.search(sentence):
                continue
            text = sentence.casefold()
            for match in pattern.finditer(text):
                if subject.search(text[:match.start()]):
                    hits.setdefault(match["value"], (sentence, number))
    if len(hits) != 1:
        return None
    sentence, number = next(iter(hits.values()))
    return f"{sentence} [{number}]"


def _fuel_yield(item, passages):
    name = re.escape(item)
    amount = r"(?:up to )?(?P<amount>[1-9][0-9]{0,2}) items?"
    direct = re.compile(
        rf"(?:a |an |one )?(?:piece of )?{name} (?:can|is able to) smelt {amount}\."
    )
    burn_time = re.compile(
        rf"(?:when used in a furnace as a fuel, )?(?:a |an |one )?(?:piece of )?{name} "
        rf"(?:burns|lasts) (?:for )?[0-9]+ seconds \(smelting {amount}\)(?:, like [a-z -]+)?\."
    )
    intro = re.compile(rf"{name}(?:s|es)? can be used as fuel in furnaces(?:, blast furnaces, and smokers)?\.")
    follows_intro = re.compile(
        rf"it is able to smelt {amount}(?:, taking [0-9,]+ ticks in a furnace"
        rf"(?:, and [0-9,]+ ticks in the blast furnace and smoker)?)?\."
    )
    hits = {}
    for number, passage in enumerate(passages, 1):
        if passage["section"] != "Fuel":
            continue
        title = passage["title"].casefold()
        if title == "smelting" and passage["text"].partition("; ")[0].casefold() == "fuel: " + item:
            # The accepted table columns carry only unqualified measurements.
            row = _FUEL_ROW.fullmatch(passage["text"])
            if not row or _EDITION_NOTE.search(row[0]):
                return None
            hits.setdefault(row["amount"], number)
        if title != item:
            continue
        previous = ""
        for sentence in re.split(r"(?<=[.!?])\s+|\n", passage["text"].casefold()):
            match = direct.fullmatch(sentence) or burn_time.fullmatch(sentence)
            if not match and intro.fullmatch(previous):
                match = follows_intro.fullmatch(sentence)
            previous = sentence
            if match:
                hits.setdefault(match["amount"], number)
    if len(hits) != 1:
        return None
    amount, number = next(iter(hits.items()))
    unit = "item" if amount == "1" else "items"
    return f"It can smelt up to {amount} {unit}. [{number}]"


def exact_answer(question, passages):
    question = " ".join(question.casefold().split()).removesuffix("?")
    # "How many slots are in a hopper?" also matches the broad count form.
    match = _SLOTS.fullmatch(question)
    if match:
        return _property(
            match["item"] or match["container"], _SLOTS_VALUE, passages,
            sections=("Container", "Storage", "Usage", "Overview"), allow_leading_condition=False,
        )
    match = _COUNT.fullmatch(question) or _RECIPE_TAKES.fullmatch(question)
    if match:
        ingredient = _singular(match["ingredient"].removeprefix("pieces of "))
        return _recipe_count(match["item"], ingredient, passages)
    match = _RECIPE_LIST.fullmatch(question)
    if match:
        return _recipe_list(match["item"], passages)
    for pattern, label, value_pattern in _OVERVIEW_FIELDS:
        match = pattern.fullmatch(question)
        if match:
            item = next(item for item in match.groups() if item is not None)
            return _overview_field(item, label, value_pattern, passages, plural=question.startswith("are "))
    match = _SIGNAL.fullmatch(question)
    if match:
        return _property(match["item"], _SIGNAL_VALUE, passages)
    match = _LIGHT.fullmatch(question)
    if match:
        return _property(match["item"], _LIGHT_VALUE, passages)
    match = _PULSE.fullmatch(question)
    if match:
        return _property(match["item"], _PULSE_VALUE, passages)
    match = _FUEL.fullmatch(question)
    if match:
        return _fuel_yield(match["item"], passages)
    return None
