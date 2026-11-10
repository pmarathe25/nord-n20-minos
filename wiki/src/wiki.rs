use crate::{
    Mode, Page, Result, api, cached_article, fetch_page,
    search::{Index, is_stop, raw_words},
};
use serde::Deserialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_QUESTION: usize = 512;
const MAX_DISCOVERY_TITLES: usize = 40;
const MAX_FETCH: usize = 2;

#[derive(Deserialize)]
struct Response {
    query: Option<Query>,
}

#[derive(Default, Deserialize)]
#[serde(default)]
struct Query {
    normalized: Vec<Alias>,
    redirects: Vec<Alias>,
    pages: Vec<WikiTitle>,
    search: Vec<WikiTitle>,
}

#[derive(Deserialize)]
struct Alias {
    from: String,
    to: String,
}

#[derive(Deserialize)]
struct WikiTitle {
    ns: i32,
    title: String,
    #[serde(default)]
    missing: bool,
    #[serde(default)]
    invalid: bool,
    #[serde(default)]
    revisions: Vec<Revision>,
}

#[derive(Deserialize)]
struct Revision {
    revid: u64,
}

impl Query {
    fn resolve(&self, title: &str) -> Option<&WikiTitle> {
        let normalized = self
            .normalized
            .iter()
            .find(|item| item.from == title)
            .map_or(title, |item| &item.to);
        let mut resolved = normalized;
        for _ in 0..=self.redirects.len() {
            let Some(redirect) = self.redirects.iter().find(|item| item.from == resolved) else {
                return self
                    .pages
                    .iter()
                    .find(|page| page.title == resolved && page.usable());
            };
            resolved = &redirect.to;
        }
        None
    }
}

impl WikiTitle {
    fn usable(&self) -> bool {
        self.ns == 0 && !self.missing && !self.invalid
    }
}

fn query(params: &[(&str, &str)]) -> Result<Query> {
    let response: Response = api(params)?;
    response
        .query
        .ok_or_else(|| "wiki response did not contain a query result".into())
}

fn phrases(words: &[String]) -> Vec<String> {
    let mut found = HashSet::new();
    let mut phrases = Vec::new();
    for len in (1..=words.len().min(5)).rev() {
        for span in words.windows(len).rev() {
            if is_stop(&span[0])
                || is_stop(&span[len - 1])
                || span
                    .iter()
                    .any(|word| word == "wiki" || word == "minecraft")
                || span
                    .iter()
                    .all(|word| word.bytes().all(|ch| ch.is_ascii_digit()))
            {
                continue;
            }
            let phrase = span.join(" ");
            if found.insert(phrase.clone()) {
                phrases.push(phrase);
            }
            if phrases.len() == MAX_DISCOVERY_TITLES {
                return phrases;
            }
        }
    }
    phrases
}

fn title_discovery(words: &[String]) -> Result<Vec<(String, String)>> {
    let phrases = phrases(words);
    if phrases.is_empty() {
        return Ok(Vec::new());
    }
    let titles = phrases.join("|");
    let matches = query(&[("action", "query"), ("titles", &titles), ("redirects", "1")])?;
    let mut seen = HashSet::new();
    Ok(phrases
        .iter()
        .filter_map(|phrase| {
            let page = matches.resolve(phrase)?;
            if page.title.contains("/History") || !seen.insert(&page.title) {
                return None;
            }
            Some((phrase.clone(), page.title.clone()))
        })
        .collect())
}

fn text_discovery(words: &[String]) -> Result<Vec<String>> {
    let mut seen = HashSet::new();
    let keywords = words
        .iter()
        .filter(|word| !is_stop(word) && seen.insert(*word))
        .take(10)
        .cloned()
        .collect::<Vec<_>>()
        .join(" ");
    if keywords.is_empty() {
        return Ok(Vec::new());
    }
    let matches = query(&[
        ("action", "query"),
        ("list", "search"),
        ("srnamespace", "0"),
        ("srlimit", "8"),
        ("srprop", ""),
        ("srsearch", &keywords),
    ])?;
    Ok(matches
        .search
        .into_iter()
        .filter(|page| {
            page.usable()
                && !page.title.starts_with("Bedrock Edition")
                && !page.title.starts_with("Java Edition ")
                && !page.title.contains("/History")
        })
        .map(|page| page.title)
        .collect())
}

pub(crate) fn evidence(
    index: &mut Index,
    cache: &Path,
    question: &str,
    offline: bool,
) -> Result<Value> {
    if question.trim().is_empty()
        || question.len() > MAX_QUESTION
        || question.chars().any(char::is_control)
    {
        return Err("expected a single question of 1 to 512 bytes".into());
    }
    let mut hits = index.find(question)?;
    let mut warnings = Vec::new();
    let mut fetched = 0;
    let words = raw_words(question);
    let key = format!("{:x}", Sha256::digest(words.join(" ").as_bytes()));
    // A single cached title can be the material in a question about a different page.
    let ambiguous = hits.title_words < 2
        && words.iter().filter(|word| !is_stop(word)).count() > hits.title_words;
    if !offline && (ambiguous || hits.passages.is_empty()) && !index.recently_discovered(&key)? {
        let mut matches = match title_discovery(&words) {
            Ok(matches) => matches,
            Err(error) => {
                warnings.push(format!("wiki title lookup: {error}"));
                Vec::new()
            }
        };
        if matches.is_empty()
            || (matches[0].0.split_whitespace().count() == 1 && hits.passages.is_empty())
        {
            match text_discovery(&words) {
                Ok(titles) => {
                    matches.extend(titles.into_iter().map(|title| (title.clone(), title)))
                }
                Err(error) => warnings.push(format!("wiki search: {error}")),
            }
        }
        let mut attempted = HashSet::new();
        for (alias, title) in matches {
            if index.has_title(&title)? || !attempted.insert(title.clone()) {
                continue;
            }
            let result = cached_article(cache, &alias, Mode::Cached, |_| fetch_page(&title));
            match result {
                Ok(_) => fetched += 1,
                Err(error) => warnings.push(format!("{title}: {error}")),
            }
            if attempted.len() >= MAX_FETCH {
                break;
            }
        }
        if fetched > 0 {
            index.sync(cache)?;
            hits = index.find(question)?;
        }
        if warnings.is_empty() {
            index.remember_discovery(&key)?;
        }
    }
    Ok(json!({"passages": hits.passages, "fetched": fetched, "warnings": warnings}))
}

pub(crate) fn update(index: &mut Index, cache: &Path, max_age_hours: u32) -> Result<Value> {
    update_with(
        index,
        cache,
        max_age_hours,
        |titles| {
            query(&[
                ("action", "query"),
                ("titles", titles),
                ("redirects", "1"),
                ("prop", "revisions"),
                ("rvprop", "ids"),
            ])
        },
        fetch_page,
    )
}

fn update_with(
    index: &mut Index,
    cache: &Path,
    max_age_hours: u32,
    latest: impl Fn(&str) -> Result<Query>,
    fetch: impl Fn(&str) -> Result<Page>,
) -> Result<Value> {
    let now = i64::try_from(SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs())?;
    let due = index.due(now - i64::from(max_age_hours) * 3600)?;
    let mut unchanged = 0;
    let mut refreshed = 0;
    let mut failed = Vec::new();
    let mut checked = HashMap::new();
    for batch in due.chunks(25) {
        let titles = batch
            .iter()
            .map(|(alias, _, _)| alias.as_str())
            .collect::<Vec<_>>()
            .join("|");
        let revisions = latest(&titles);
        let revisions = match revisions {
            Ok(revisions) => revisions,
            Err(error) => {
                for (alias, _, _) in batch {
                    failed.push(json!({"title": alias, "error": error.to_string()}));
                }
                continue;
            }
        };
        for (alias, title, revision) in batch {
            let latest = revisions
                .resolve(alias)
                .and_then(|page| Some((&page.title, page.revisions.first()?.revid)));
            match latest {
                Some((canonical, latest)) if title == canonical && *revision == latest => {
                    unchanged += 1;
                    checked.insert(alias, *revision);
                }
                Some(_) => match cached_article(cache, alias, Mode::Refresh, &fetch) {
                    Ok(article) => {
                        refreshed += 1;
                        checked.insert(alias, article.revision);
                    }
                    Err(error) => failed.push(json!({"title": alias, "error": error.to_string()})),
                },
                None => failed.push(
                    json!({"title": alias, "error": "wiki did not provide a current revision"}),
                ),
            }
        }
    }
    let indexed = index.sync(cache)?;
    for (alias, revision) in checked {
        index.checked(alias, revision, now)?;
    }
    Ok(
        json!({"due": due.len(), "unchanged": unchanged, "refreshed": refreshed,
        "indexed": indexed, "failed": failed}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn page(revid: u64, fact: &str) -> Page {
        Page {
            title: "Hopper".into(),
            revid,
            text: format!(r#"<div class="mw-parser-output"><p>{fact}</p></div>"#),
        }
    }

    fn latest(revid: u64) -> Query {
        serde_json::from_value(json!({
            "normalized": [{"from":"hopper","to":"Hopper"}],
            "pages": [{"ns":0,"title":"Hopper","revisions":[{"revid":revid}]}],
        }))
        .unwrap()
    }

    #[test]
    fn indexes_existing_cache_and_refreshes_only_changed_revisions() {
        let cache = tempfile::tempdir().unwrap();
        let cache = cache.path();
        cached_article(cache, "hopper", Mode::Cached, |_| {
            Ok(page(101, "A hopper holds five slots."))
        })
        .unwrap();
        let mut index = Index::open(cache).unwrap();
        assert_eq!(index.sync(cache).unwrap(), 1);
        assert_eq!(index.sync(cache).unwrap(), 0);
        let initial = evidence(&mut index, cache, "How many slots are in a hopper?", true).unwrap();
        assert_eq!(initial["passages"][0]["revision"], 101);
        assert_eq!(initial["fetched"], 0);

        let same = update_with(
            &mut index,
            cache,
            0,
            |_| Ok(latest(101)),
            |_| panic!("unchanged page fetched"),
        )
        .unwrap();
        assert_eq!(same["unchanged"], 1);
        assert_eq!(same["indexed"], 0);
        let not_due = update_with(
            &mut index,
            cache,
            24,
            |_| panic!("fresh page checked"),
            |_| panic!("fresh page fetched"),
        )
        .unwrap();
        assert_eq!(not_due["due"], 0);

        let changed = update_with(
            &mut index,
            cache,
            0,
            |_| Ok(latest(102)),
            |_| Ok(page(102, "A hopper holds five inventory slots.")),
        )
        .unwrap();
        assert_eq!(changed["refreshed"], 1);
        assert_eq!(changed["indexed"], 1);
        let failed = update_with(
            &mut index,
            cache,
            0,
            |_| Ok(latest(103)),
            |_| Err("network error".into()),
        )
        .unwrap();
        assert_eq!(failed["failed"].as_array().unwrap().len(), 1);
        assert_eq!(failed["indexed"], 0);
        let current = evidence(&mut index, cache, "How many slots are in a hopper?", true).unwrap();
        assert_eq!(current["passages"][0]["revision"], 102);
        assert_eq!(
            current["passages"][0]["text"],
            "A hopper holds five inventory slots."
        );
    }
}
