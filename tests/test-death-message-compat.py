"""Check death-message parsing against an extracted Minecraft server JAR."""
import argparse
import json
from pathlib import Path
import re
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/lib/n20'))
from wiki_chat import compile_death_message_patterns


def render(template, player):
    values = [player, 'Zombie', '[Iron Sword]']
    automatic = 0

    def replace(match):
        nonlocal automatic
        if match[1]:
            index = int(match[1]) - 1
        else:
            index = automatic
            automatic += 1
        if not 0 <= index < len(values):
            raise ValueError('unsupported placeholder')
        return values[index]

    return re.sub(r'%(?:(\d+)\$)?s', replace, template)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jar', type=Path, required=True, help='extracted versioned server JAR')
    args = parser.parse_args()
    with zipfile.ZipFile(args.jar) as jar:
        language = json.loads(jar.read('assets/minecraft/lang/en_us.json'))
    patterns = compile_death_message_patterns(language)
    # These are a link label and a client-side message-length warning, not
    # individual player death messages.
    excluded = {'death.attack.badRespawnPoint.link', 'death.attack.message_too_long'}
    templates = {key: value for key, value in language.items()
                 if key.startswith('death.') and key not in excluded}
    if not templates:
        raise SystemExit('No death-message templates found')
    failures = []
    for key, template in sorted(templates.items()):
        if not isinstance(template, str):
            failures.append(key + ': not a text template')
            continue
        for player in ('Alex', '[Team] Alex'):
            try:
                message = render(template, player)
            except ValueError:
                failures.append(key + ': unsupported placeholder')
                break
            if message == template or re.search(r'%(?:(\d+)\$)?[a-zA-Z]', message):
                failures.append(key + ': unsupported or missing placeholder')
                break
            matches = [m for pattern in patterns if (m := pattern.fullmatch(message))]
            if not any(match['player'] == player for match in matches):
                failures.append(key + ': listener did not recognize the player')
                break
    if failures:
        raise SystemExit('\n'.join(failures))
    print(f'All {len(templates)} server death-message templates matched.')


if __name__ == '__main__':
    main()
