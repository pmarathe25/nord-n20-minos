use crate::{CacheEntry, Result, parser::Block, read_entry};
use regex::Regex;
use rusqlite::{Connection, OptionalExtension, Transaction, TransactionBehavior, params};
use serde::Serialize;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::Path;
use std::sync::LazyLock;
use std::time::{Duration, UNIX_EPOCH};

const PASSAGE_BYTES: usize = 750;
const CONTEXT_BYTES: usize = 1800;
const INDEX_FILE: &str = "passages-v8.sqlite3";

static WORD: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"[a-zA-Z]+|\d+(?:\.\d+)?").unwrap());
static SENTENCE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r#"[.!?]["”]?\s+"#).unwrap());
static QUESTION_COUNT: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"(?i)\b(how (many|much|long|strong|far)|at most|maximum|minimum|highest|lowest|total)\b",
    )
    .unwrap()
});
const STOP: &str = "a an and are as at be been being by can could did do does for from give get got had has have how i if in into is it its just me much my of on one or our per put so than that the their them then there these they this to too up use using was were what when where which who why will with would you your";

pub(crate) fn raw_words(text: &str) -> Vec<String> {
    WORD.find_iter(text)
        .map(|m| m.as_str().to_ascii_lowercase())
        .collect()
}

pub(crate) fn is_stop(word: &str) -> bool {
    word == "s" || STOP.split_ascii_whitespace().any(|stop| stop == word)
}

fn words(text: &str) -> Vec<String> {
    raw_words(text)
        .into_iter()
        .filter(|word| !is_stop(word))
        .map(|word| {
            let stem = match word.as_str() {
                "xp" | "exp" => "experience",
                "powered" => "power",
                "highest" => "maximum",
                "lowest" => "minimum",
                "regeneration" | "regenerates" | "regenerate" | "recover" | "heals" | "healed"
                | "healing" => "heal",
                "renamed" | "rename" | "naming" | "named" => "name",
                "storage" | "stored" | "storing" => "store",
                "crafting" | "crafted" | "crafts" => "craft",
                "build" | "built" | "building" | "created" | "creation" | "creating" => "create",
                "slot" | "slots" | "fit" | "fits" => "capacity",
                "activates" | "activated" | "activating" | "activation" => "activate",
                "moved" | "moving" | "moves" => "move",
                "pushed" | "pushing" | "pushes" => "push",
                "detection" | "detected" | "detects" | "watch" | "watches" | "watching" => "detect",
                "redirect" | "redirects" | "redirected" | "diverting" | "diverted" | "diverts" => {
                    "divert"
                }
                "summons" | "summoned" | "summoning" => "summon",
                "become" | "becomes" | "became" | "turns" | "turned" => "turn",
                "breaking" | "broke" | "broken" | "smash" | "smashing" => "break",
                "range" | "distance" => "radius",
                "compatibility" | "incompatibility" | "incompatibilities" | "incompatible" => {
                    "compatible"
                }
                "waterlogged" | "waterlogging" | "waterloggable" => "waterlog",
                "dropped" | "dropping" => "drop",
                "killed" | "kills" | "killing" | "death" => "kill",
                "died" | "dies" | "dying" => "die",
                "falling" | "falls" | "floating" | "float" => "fall",
                "under" | "beneath" => "below",
                "soak" | "soaks" | "soaked" | "absorption" | "absorbs" | "absorbed" => "absorb",
                "carving" | "carved" => "carve",
                "melting" | "melted" => "melt",
                "smelting" | "smelted" => "smelt",
                "picked" | "picking" | "collect" | "collected" | "collecting" | "scoop" => "pick",
                "brewing" | "brewed" => "brew",
                "bred" | "breeding" => "breed",
                "enchanted" | "enchantment" | "enchantments" | "enchanting" => "enchant",
                _ if word.len() > 4
                    && word.ends_with('s')
                    && !["ss", "us", "is"]
                        .iter()
                        .any(|ending| word.ends_with(ending)) =>
                {
                    &word[..word.len() - 1]
                }
                _ => &word,
            };
            stem.to_owned()
        })
        .collect()
}

fn contains_phrase(query: &[String], phrase: &[String]) -> bool {
    !phrase.is_empty() && query.windows(phrase.len()).any(|window| window == phrase)
}

fn query_expansions(question: &str) -> Vec<String> {
    let question = question.to_ascii_lowercase();
    let mut expanded: Vec<String> = [
        ("power level", "signal strength"),
        ("comes out", "output"),
        ("come out", "output"),
        ("what variant", "turn"),
    ]
    .iter()
    .filter(|(phrase, _)| question.contains(phrase))
    .flat_map(|(_, expansion)| words(expansion))
    .collect();
    if raw_words(&question)
        .iter()
        .any(|word| matches!(word.as_str(), "powered" | "powering"))
    {
        expanded.extend(words("redstone signal"));
    }
    expanded
}

#[derive(Serialize)]
pub(crate) struct Passage {
    pub title: String,
    pub revision: u64,
    pub section: String,
    pub anchor: String,
    pub text: String,
}

struct Candidate {
    passage: Passage,
    alias: String,
    overview: bool,
    bm25: f64,
    terms: String,
}

pub(crate) struct Hits {
    pub passages: Vec<Passage>,
    pub title_words: usize,
}

pub(crate) struct Index {
    db: Connection,
}

impl Index {
    pub fn exists(cache: &Path) -> bool {
        cache.join(INDEX_FILE).is_file()
    }

    pub fn open(cache: &Path) -> Result<Self> {
        fs::create_dir_all(cache)?;
        let db = Connection::open(cache.join(INDEX_FILE))?;
        db.busy_timeout(Duration::from_secs(15))?;
        db.execute_batch(
            "CREATE TABLE IF NOT EXISTS pages (
                 id INTEGER PRIMARY KEY, filename TEXT NOT NULL UNIQUE,
                 stamp TEXT NOT NULL, title TEXT NOT NULL, alias TEXT NOT NULL,
                 revision INTEGER NOT NULL, checked INTEGER NOT NULL
             );
             CREATE VIRTUAL TABLE IF NOT EXISTS passages USING fts5(
                 title, heading, terms, text UNINDEXED, anchor UNINDEXED,
                 page UNINDEXED, overview UNINDEXED
             );
             CREATE TABLE IF NOT EXISTS discovery (
                 key TEXT PRIMARY KEY, checked INTEGER NOT NULL
             );",
        )?;
        Ok(Self { db })
    }

    pub fn sync(&mut self, cache: &Path) -> Result<usize> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let mut known = {
            let mut statement = tx.prepare("SELECT filename, id, stamp FROM pages")?;
            statement
                .query_map([], |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        (row.get::<_, i64>(1)?, row.get::<_, String>(2)?),
                    ))
                })?
                .collect::<rusqlite::Result<HashMap<_, _>>>()?
        };
        let mut changed = 0;
        for file in fs::read_dir(cache)? {
            let file = file?;
            let name = file.file_name();
            let Some(name) = name.to_str().filter(|name| is_cache_file(name)) else {
                continue;
            };
            if !file.file_type()?.is_file() {
                continue;
            }
            let old = known.remove(name);
            let meta = match file.metadata() {
                Ok(meta) => meta,
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    if let Some((id, _)) = old {
                        remove(&tx, id)?;
                    }
                    continue;
                }
                Err(error) => return Err(error.into()),
            };
            let stamp = format!(
                "{}:{}",
                meta.len(),
                meta.modified()?.duration_since(UNIX_EPOCH)?.as_nanos()
            );
            if old
                .as_ref()
                .is_some_and(|(_, old_stamp)| *old_stamp == stamp)
            {
                continue;
            }
            if let Some((id, _)) = old {
                remove(&tx, id)?;
            }
            let Ok(entry) = read_entry(&file.path()) else {
                // The title command can repair broken entries from the network.
                continue;
            };
            add(&tx, name, &stamp, entry)?;
            changed += 1;
        }
        for (_, (id, _)) in known {
            remove(&tx, id)?;
        }
        tx.commit()?;
        Ok(changed)
    }

    pub fn has_title(&self, title: &str) -> Result<bool> {
        Ok(self.db.query_row(
            "SELECT 1 FROM pages WHERE title = ?1 COLLATE NOCASE OR alias = ?1 COLLATE NOCASE LIMIT 1",
            [title], |_| Ok(()),
        ).optional()?.is_some())
    }

    pub fn recently_discovered(&self, key: &str) -> Result<bool> {
        Ok(self
            .db
            .query_row(
                "SELECT 1 FROM discovery WHERE key = ?1 AND checked > unixepoch() - 86400",
                [key],
                |_| Ok(()),
            )
            .optional()?
            .is_some())
    }

    pub fn remember_discovery(&self, key: &str) -> Result<()> {
        self.db.execute(
            "DELETE FROM discovery WHERE checked <= unixepoch() - 86400",
            [],
        )?;
        self.db.execute(
            "INSERT INTO discovery VALUES (?1, unixepoch()) ON CONFLICT(key)
             DO UPDATE SET checked = excluded.checked",
            [key],
        )?;
        Ok(())
    }

    pub fn due(&self, cutoff: i64) -> Result<Vec<(String, String, u64)>> {
        let mut statement = self.db.prepare(
            "SELECT alias, title, revision FROM pages WHERE checked <= ?1 ORDER BY checked, id",
        )?;
        Ok(statement
            .query_map([cutoff], |row| {
                Ok((row.get(0)?, row.get(1)?, row.get::<_, i64>(2)? as u64))
            })?
            .collect::<rusqlite::Result<_>>()?)
    }

    pub fn checked(&self, alias: &str, revision: u64, time: i64) -> Result<()> {
        self.db.execute(
            "UPDATE pages SET checked = ?1 WHERE alias = ?2 AND revision = ?3",
            params![time, alias, i64::try_from(revision)?],
        )?;
        Ok(())
    }

    pub fn find(&self, question: &str) -> Result<Hits> {
        let tokens = words(question);
        let expansions = query_expansions(question);
        let terms: HashSet<_> = tokens
            .iter()
            .chain(&expansions)
            .map(String::as_str)
            .collect();
        if terms.is_empty() {
            return Ok(Hits {
                passages: Vec::new(),
                title_words: 0,
            });
        }
        let query = tokens
            .iter()
            .chain(&expansions)
            .take(32)
            .map(|word| format!("\"{word}\""))
            .collect::<Vec<_>>()
            .join(" OR ");
        let mut statement = self.db.prepare_cached(
            "SELECT p.title, p.revision, a.heading, a.anchor, a.text, p.alias,
                    a.overview, bm25(passages, 2.5, 1.3, 1.0), a.terms
             FROM passages AS a JOIN pages AS p ON p.id = a.page
             WHERE passages MATCH ?1
             ORDER BY 8, a.rowid LIMIT 192",
        )?;
        let candidates = statement
            .query_map([query], |row| {
                Ok(Candidate {
                    passage: Passage {
                        title: row.get(0)?,
                        revision: row.get::<_, i64>(1)? as u64,
                        section: row.get(2)?,
                        anchor: row.get(3)?,
                        text: row.get(4)?,
                    },
                    alias: row.get(5)?,
                    overview: row.get(6)?,
                    bm25: -row.get::<_, f64>(7)?,
                    terms: row.get(8)?,
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rank(candidates, question, &tokens, &terms))
    }
}

fn is_cache_file(name: &str) -> bool {
    name.strip_suffix(".json")
        .is_some_and(|stem| stem.len() == 64 && stem.bytes().all(|ch| ch.is_ascii_hexdigit()))
}

fn remove(tx: &Transaction<'_>, id: i64) -> Result<()> {
    tx.execute("DELETE FROM passages WHERE page = ?1", [id])?;
    tx.execute("DELETE FROM pages WHERE id = ?1", [id])?;
    Ok(())
}

fn add(tx: &Transaction<'_>, filename: &str, stamp: &str, entry: CacheEntry) -> Result<()> {
    let article = entry.article;
    tx.execute(
        "INSERT INTO pages(filename, stamp, title, alias, revision, checked)
         VALUES (?1, ?2, ?3, ?4, ?5, CAST(substr(?2, instr(?2, ':') + 1) AS INTEGER) / 1000000000)",
        params![
            filename,
            stamp,
            article.title,
            entry.requested_title,
            i64::try_from(article.revision)?
        ],
    )?;
    let id = tx.last_insert_rowid();
    let title = words(&format!("{} {}", article.title, entry.requested_title)).join(" ");
    let mut insert = tx.prepare_cached(
        "INSERT INTO passages(title, heading, terms, text, anchor, page, overview)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
    )?;
    // Keep the normalized heading indexed and retain the original heading for citations.
    for section in article
        .sections
        .into_iter()
        .filter(|section| section.default)
    {
        let heading = format!("{}\n{}", section.heading, words(&section.heading).join(" "));
        let mut parents: Vec<(usize, String)> = Vec::new();
        for block in section.blocks {
            let parts = match block {
                Block::Text { text } => {
                    parents.clear();
                    snippets(&text)
                }
                Block::List { text, depth } => {
                    parents.retain(|(level, _)| *level < depth);
                    let passage = match parents.last() {
                        Some((_, parent)) => format!("{parent}\n{text}"),
                        None => text.clone(),
                    };
                    parents.push((depth, text));
                    vec![passage]
                }
                Block::Row { text } => {
                    parents.clear();
                    vec![text]
                }
            };
            for text in parts
                .into_iter()
                .filter(|text| !text.is_empty() && text.len() <= PASSAGE_BYTES)
            {
                insert.execute(params![
                    title,
                    heading,
                    words(&text).join(" "),
                    text,
                    section.anchor,
                    id,
                    section.heading == "Overview"
                        && matches!(text.as_bytes().last(), Some(b'.' | b'!' | b'?')),
                ])?;
            }
        }
    }
    Ok(())
}

fn snippets(text: &str) -> Vec<String> {
    let mut sentences = Vec::new();
    let mut start = 0;
    for boundary in SENTENCE.find_iter(text) {
        if text[boundary.end()..]
            .chars()
            .next()
            .is_some_and(|ch| ch.is_uppercase() || ch.is_ascii_digit())
        {
            sentences.push(text[start..boundary.end()].trim());
            start = boundary.end();
        }
    }
    sentences.push(text[start..].trim());
    let mut out = Vec::new();
    for (i, &sentence) in sentences.iter().enumerate() {
        if i > 0
            && dependent(sentence)
            && !dependent(sentences[i - 1])
            && sentences[i - 1].len() + 1 + sentence.len() <= PASSAGE_BYTES
        {
            continue;
        }
        let following = sentences.get(i + 1).copied().unwrap_or_default();
        let qualified = [
            "If ",
            "Unless ",
            "However,",
            "Otherwise,",
            "When ",
            "Each ",
            "It ",
            "In Java Edition,",
        ]
        .iter()
        .any(|prefix| following.starts_with(prefix))
            || dependent(following);
        if qualified && sentence.len() + 1 + following.len() <= PASSAGE_BYTES {
            out.push(format!("{sentence} {following}"));
        } else {
            out.push(sentence.to_owned());
        }
    }
    if sentences.len() > 1 && text.len() <= PASSAGE_BYTES && !out.iter().any(|part| part == text) {
        out.push(text.to_owned());
    }
    out
}

// These clauses refer to the rule or entity named in the preceding sentence.
fn dependent(sentence: &str) -> bool {
    [
        "This ",
        "These ",
        "Otherwise,",
        "If broken without the ",
        "If mined without the ",
    ]
    .iter()
    .any(|prefix| sentence.starts_with(prefix))
}

fn rank(
    candidates: Vec<Candidate>,
    question: &str,
    tokens: &[String],
    terms: &HashSet<&str>,
) -> Hits {
    let count = QUESTION_COUNT.is_match(question)
        || tokens.iter().any(|word| {
            word.as_bytes()[0].is_ascii_digit()
                || matches!(
                    word.as_str(),
                    "two"
                        | "three"
                        | "four"
                        | "five"
                        | "six"
                        | "seven"
                        | "eight"
                        | "nine"
                        | "ten"
                        | "eleven"
                        | "twelve"
                        | "thirteen"
                )
        });
    let making = ["craft", "make", "turn", "create", "build", "carve"]
        .iter()
        .any(|word| terms.contains(word));
    let q = question.to_ascii_lowercase();
    let asks_usage = q.contains("used for")
        || q.contains("use for")
        || q.contains("what does")
        || q.contains("what can i do with");
    let minimum = ["minimum", "start", "enough", "least"]
        .iter()
        .any(|word| terms.contains(word));
    let asks_about_breaking = [
        "break", "mine", "mining", "pickaxe", "hardness", "drop", "harvest",
    ]
    .iter()
    .any(|word| terms.contains(word));
    let mut title_words = 0;
    let mut ranked = Vec::new();
    for mut item in candidates {
        let title = words(&item.passage.title);
        let alias = words(&item.alias);
        let match_len = [&title, &alias]
            .into_iter()
            .filter(|phrase| contains_phrase(tokens, phrase))
            .map(Vec::len)
            .max()
            .unwrap_or(0);
        let shared_title = title
            .iter()
            .filter(|word| terms.contains(word.as_str()))
            .count();
        title_words = title_words.max(match_len);
        let focus: HashSet<_> = terms
            .iter()
            .copied()
            .filter(|term| !title.iter().any(|w| w == term) && !alias.iter().any(|w| w == term))
            .collect();
        let text_terms: HashSet<_> = item.terms.split_ascii_whitespace().collect();
        let matched: HashSet<_> = focus.intersection(&text_terms).copied().collect();
        let hits: f64 = matched.iter().map(|term| term_weight(term)).sum();
        let heading = item
            .passage
            .section
            .split_once('\n')
            .unwrap_or((&item.passage.section, ""))
            .0
            .to_owned();
        let heading_hits = words(&heading)
            .iter()
            .filter(|w| focus.contains(w.as_str()))
            .count() as f64;
        item.passage.section = heading;
        let numeric = count
            && hits > 0.0
            && text_terms.iter().any(|word| {
                word.as_bytes()[0].is_ascii_digit()
                    || matches!(
                        *word,
                        "two" | "three" | "four" | "five" | "eight" | "ten" | "twelve"
                    )
            });
        let result_type = item.passage.section.to_ascii_lowercase();
        let describes_creation = making
            && shared_title > 0
            && (result_type == "crafting"
                || result_type == "smelting"
                || result_type == "obtaining"
                || result_type == "cutting"
                || result_type == "creation"
                || result_type == "construction"
                || item.overview && item.passage.text.contains("obtained by"));
        let score = 4.0 * match_len.min(4) as f64
            + 1.25 * shared_title.min(3) as f64
            + 2.7 * hits
            + 1.6 * heading_hits
            + item.bm25.min(24.0) * 0.26
            + 2.8 * f64::from(numeric)
            + 2.3 * f64::from(minimum && numeric && text_terms.contains("minimum"))
            + 3.0 * f64::from(describes_creation)
            - 3.0 * f64::from(making && result_type == "usage")
            + 8.0
                * f64::from(
                    asks_usage
                        && shared_title > 0
                        && (result_type == "usage" || result_type == "smithing ingredient"),
                )
            + 2.0 * f64::from(asks_usage && !item.passage.text.starts_with("Ingredients:"))
            + 3.0
                * f64::from(
                    item.overview
                        && shared_title > 0
                        && ["A ", "An ", "The "]
                            .iter()
                            .any(|prefix| item.passage.text.starts_with(prefix)),
                )
            - 2.0 * f64::from(item.passage.text.starts_with("It "))
            + 1.5 * f64::from(item.overview)
            - 4.0
                * f64::from(
                    result_type == "breaking"
                        && !asks_about_breaking
                        && item.passage.text.starts_with("Block: "),
                )
            - item.passage.text.len() as f64 / 520.0
            - 0.55 * item.passage.text.matches(" or ").count().min(10) as f64;
        if match_len > 0 || hits > 0.0 || heading_hits > 0.0 {
            let matched = matched
                .into_iter()
                .map(str::to_owned)
                .collect::<HashSet<_>>();
            ranked.push((score, item.passage, matched, match_len));
        }
    }
    ranked.sort_by(|a, b| b.0.total_cmp(&a.0));
    let mut selected: Vec<Passage> = Vec::new();
    let mut covered = HashSet::new();
    let mut remaining = CONTEXT_BYTES;
    while selected.len() < 4 {
        let primary = selected.first();
        let primary_exact =
            primary.is_some_and(|first| contains_phrase(tokens, &words(&first.title)));
        let best = ranked
            .iter()
            .enumerate()
            .filter_map(|(i, (score, passage, matched, match_len))| {
                if passage.text.len() > remaining
                    || selected.iter().any(|prior| {
                        prior.title == passage.title
                            && (prior.revision != passage.revision
                                || prior.text.contains(&passage.text)
                                || passage.text.contains(&prior.text))
                    })
                {
                    return None;
                }
                let novel: f64 = matched
                    .difference(&covered)
                    .map(|term| term_weight(term))
                    .sum();
                let follows_topic =
                    novel > 0.0 && primary.is_some_and(|first| first.title == passage.title);
                let distractor = *match_len == 0
                    && primary_exact
                    && primary.is_some_and(|first| first.title != passage.title);
                Some((
                    i,
                    score + 2.9 * novel + 3.0 * f64::from(follows_topic)
                        - 3.0 * f64::from(distractor),
                ))
            })
            .max_by(|a, b| a.1.total_cmp(&b.1).then_with(|| b.0.cmp(&a.0)));
        let Some((i, _)) = best else { break };
        let (_, passage, matched, _) = ranked.remove(i);
        covered.extend(matched);
        remaining -= passage.text.len();
        selected.push(passage);
    }
    Hits {
        passages: selected,
        title_words,
    }
}

fn term_weight(term: &str) -> f64 {
    match term {
        "block" | "item" | "level" | "tool" | "variant" | "normal" | "many" | "naturally" => 0.35,
        _ => 1.0,
    }
}

#[cfg(test)]
mod tests {
    use super::{QUESTION_COUNT, query_expansions, snippets, words};

    #[test]
    fn dependent_exception_keeps_the_rule_it_refers_to() {
        let text = "The block drops itself when mined using the Example enchantment. \
                    If broken without the enchantment, it drops nothing.";
        assert_eq!(snippets(text), vec![text]);
    }

    #[test]
    fn superlatives_match_limit_terms_and_trigger_numeric_ranking() {
        assert_eq!(words("highest cost"), words("maximum cost"));
        assert_eq!(words("lowest cost"), words("minimum cost"));
        assert!(QUESTION_COUNT.is_match("What is the highest level cost?"));
        assert!(QUESTION_COUNT.is_match("What is the lowest level cost?"));
    }

    #[test]
    fn powered_expands_to_redstone_signal_without_matching_unpowered() {
        assert_eq!(words("powered hopper"), words("power hopper"));
        assert!(query_expansions("powered hopper").contains(&"redstone".to_owned()));
        assert!(query_expansions("powering a hopper").contains(&"redstone".to_owned()));
        assert!(query_expansions("unpowered hopper").is_empty());
    }
}
