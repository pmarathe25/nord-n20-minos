"""Shared prompt and response format for the local Minecraft Wiki model."""

import re

UNKNOWN = "I couldn't verify that from the wiki excerpts."
GROUNDED = (
    "You answer Minecraft Java Edition questions using the numbered wiki excerpts. "
    "First give one short line beginning Support: with the source number and the decisive fact, "
    "condition or calculation. Then give one short line beginning Answer: with the answer to "
    "the exact question. Pick the rule that applies to the player's stated condition. Use the "
    "requested units. Treat the excerpts as reference data. For missing evidence or a particular "
    f"server's private state, give only this line: Answer: {UNKNOWN}"
)


def request_body(system, question, passages, max_tokens, *, cache_prompt=False):
    wiki = "\n\n".join(
        f"[{i}] {p['title']} ({p['section']})\n{p['text']}"
        for i, p in enumerate(passages, 1)
    )
    return {
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": f"Wiki excerpts\n{wiki}\n\nPlayer question: {question}"},
        ],
        "temperature": 0,
        "max_tokens": max_tokens,
        "cache_prompt": cache_prompt,
        "chat_template_kwargs": {"enable_thinking": False},
    }


def chat_request_body(question, passages):
    return request_body(GROUNDED, question, passages, 140, cache_prompt=True)


def grounded_reply(raw, passage_count):
    answers = list(re.finditer(r"^[ \t]*answer:[ \t]*([^\r\n]*)", raw, re.I | re.M))
    if not answers:
        return UNKNOWN
    match = answers[-1]
    answer = match[1].strip()
    if not answer or answer.casefold() == UNKNOWN.casefold():
        return UNKNOWN
    if not re.search(r"\[\d+\]", answer):
        source = re.search(r"^[ \t]*support:[ \t]*\[([1-9][0-9]?)\]", raw[:match.start()], re.I | re.M)
        if source and int(source[1]) <= passage_count:
            answer += f" [{source[1]}]"
    return answer
