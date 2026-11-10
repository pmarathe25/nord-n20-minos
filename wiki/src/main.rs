mod parser;
mod search;
mod wiki;

use clap::Parser;
use parser::{Article, Page};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::error::Error;
use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

const API: &str = "https://minecraft.wiki/api.php";
const MAX_BYTES: usize = 8 * 1024 * 1024;
const CACHE_VERSION: u32 = 1;
type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Parser)]
#[command(about = "Retrieve cleaned Java Edition passages from a local Minecraft Wiki cache")]
struct Args {
    /// An article title, or a player question with --question.
    #[arg(required_unless_present_any = ["batch", "index", "update"], conflicts_with_all = ["batch", "index", "update"])]
    input: Option<String>,
    /// Directory for parsed articles.
    #[arg(long)]
    cache: PathBuf,
    /// Read cached data only.
    #[arg(long, conflicts_with = "refresh")]
    offline: bool,
    /// Fetch a fresh revision and replace the cached article after parsing succeeds.
    #[arg(long)]
    refresh: bool,
    /// Include the selected section and its descendants. An empty anchor selects the overview.
    #[arg(long, conflicts_with = "list")]
    section: Option<String>,
    /// List section anchors without passage text.
    #[arg(long)]
    list: bool,
    /// Find ranked evidence for a question without supplying an article title.
    #[arg(long, conflicts_with_all = ["section", "list", "refresh", "batch", "index", "update"])]
    question: bool,
    /// Read JSON string questions on stdin and write one JSON result per line.
    #[arg(long, conflicts_with_all = ["section", "list", "refresh", "index", "update"])]
    batch: bool,
    /// Index new and changed parsed files without contacting the wiki.
    #[arg(long, conflicts_with_all = ["section", "list", "refresh", "update"])]
    index: bool,
    /// Check and refresh cached articles that are due for a revision check.
    #[arg(long, conflicts_with_all = ["offline", "section", "list", "refresh"])]
    update: bool,
    /// Age in hours before checking a cached article for a newer wiki revision.
    #[arg(long, requires = "update")]
    max_age_hours: Option<u32>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Cached,
    Offline,
    Refresh,
}

#[derive(Serialize, Deserialize)]
struct CacheEntry {
    version: u32,
    requested_title: String,
    article: Article,
}

fn read_bounded(reader: impl Read) -> Result<Vec<u8>> {
    let mut data = Vec::new();
    reader.take((MAX_BYTES + 1) as u64).read_to_end(&mut data)?;
    if data.len() > MAX_BYTES {
        return Err("article exceeds size limit".into());
    }
    Ok(data)
}

fn normalize_title(title: &str) -> Result<String> {
    if title.chars().any(char::is_control) {
        return Err("invalid article title".into());
    }
    let title = title.replace('_', " ");
    let normalized = title.split_whitespace().collect::<Vec<_>>().join(" ");
    if normalized.is_empty() || normalized.len() > 255 || normalized.contains('#') {
        return Err("expected an article title without a section fragment".into());
    }
    Ok(normalized)
}

fn read_entry(path: &Path) -> Result<CacheEntry> {
    let entry: CacheEntry = serde_json::from_slice(&read_bounded(File::open(path)?)?)?;
    if entry.version != CACHE_VERSION || entry.article.revision == 0 {
        return Err("incompatible cached article".into());
    }
    Ok(entry)
}

fn read_cache(path: &Path, title: &str) -> Result<Article> {
    let entry = read_entry(path)?;
    if entry.requested_title != title {
        return Err("incompatible cached article title".into());
    }
    Ok(entry.article)
}

fn cached_article(
    directory: &Path,
    title: &str,
    mode: Mode,
    fetch: impl FnOnce(&str) -> Result<Page>,
) -> Result<Article> {
    let key = format!("{:x}", Sha256::digest(title.as_bytes()));
    let path = directory.join(format!("{key}.json"));
    match mode {
        Mode::Offline => {
            return read_cache(&path, title).map_err(|e| format!("cache unavailable: {e}").into());
        }
        Mode::Cached => {
            if let Ok(article) = read_cache(&path, title) {
                return Ok(article);
            }
        }
        Mode::Refresh => (),
    }

    fs::create_dir_all(directory)?;
    let lock = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(false)
        .open(directory.join(format!("{key}.lock")))?;
    lock.lock()?;
    if mode == Mode::Cached
        && let Ok(article) = read_cache(&path, title)
    {
        return Ok(article);
    }

    let article = parser::extract(fetch(title)?)?;
    let entry = CacheEntry {
        version: CACHE_VERSION,
        requested_title: title.to_owned(),
        article,
    };
    let data = serde_json::to_vec(&entry)?;
    if data.len() > MAX_BYTES {
        return Err("parsed article exceeds cache size limit".into());
    }
    let mut file = tempfile::NamedTempFile::new_in(directory)?;
    file.write_all(&data)?;
    file.as_file().sync_all()?;
    file.persist(path)?;
    Ok(entry.article)
}

fn api<T: DeserializeOwned>(params: &[(&str, &str)]) -> Result<T> {
    #[derive(Deserialize)]
    struct Response<T> {
        #[serde(flatten)]
        payload: T,
        error: Option<ApiError>,
    }
    #[derive(Deserialize)]
    struct ApiError {
        code: String,
    }

    let config = ureq::Agent::config_builder()
        .max_redirects(0)
        .https_only(true)
        .timeout_global(Some(Duration::from_secs(15)))
        .build();
    let agent = ureq::Agent::new_with_config(config);
    let mut request = agent
        .get(API)
        .header(
            "User-Agent",
            "Nord-N20-MinOS-wiki/0.1 (on-demand article cache)",
        )
        .query("format", "json")
        .query("formatversion", "2");
    for &(key, value) in params {
        request = request.query(key, value);
    }
    let mut response = request.call()?;
    if !response.status().is_success() {
        return Err(format!("wiki returned HTTP {}", response.status()).into());
    }
    let body: Response<T> =
        serde_json::from_slice(&read_bounded(response.body_mut().as_reader())?)?;
    if let Some(error) = body.error {
        return Err(format!("wiki API: {}", error.code).into());
    }
    Ok(body.payload)
}

fn fetch_page(title: &str) -> Result<Page> {
    #[derive(Deserialize)]
    struct Response {
        parse: Option<Page>,
    }
    let body: Response = api(&[
        ("action", "parse"),
        ("page", title),
        ("redirects", "1"),
        ("disableeditsection", "1"),
        ("prop", "text|revid"),
    ])?;
    body.parse
        .ok_or_else(|| "wiki response did not contain an article".into())
}

fn run(args: Args) -> Result<()> {
    let stdout = io::stdout().lock();
    let mut writer = io::BufWriter::new(stdout);
    if args.batch || args.question || args.index || args.update {
        let mut index = search::Index::open(&args.cache)?;
        let indexed = index.sync(&args.cache)?;
        if args.index {
            return write_json(&mut writer, &json!({"indexed": indexed}));
        }
        if args.update {
            let report = wiki::update(&mut index, &args.cache, args.max_age_hours.unwrap_or(168))?;
            write_json(&mut writer, &report)?;
            if report["failed"]
                .as_array()
                .is_some_and(|failed| !failed.is_empty())
            {
                return Err("one or more article checks failed; see the update report".into());
            }
            return Ok(());
        }
        if args.batch {
            for line in io::stdin().lock().lines() {
                let line = line?;
                let response = serde_json::from_str::<String>(&line)
                    .map_err(Into::into)
                    .and_then(|question| {
                        wiki::evidence(&mut index, &args.cache, &question, args.offline)
                    })
                    .unwrap_or_else(|error| json!({"error": error.to_string()}));
                write_json(&mut writer, &response)?;
            }
            return Ok(());
        }
        let question = args.input.as_deref().expect("question argument required");
        return write_json(
            &mut writer,
            &wiki::evidence(&mut index, &args.cache, question, args.offline)?,
        );
    }

    let title = normalize_title(args.input.as_deref().expect("title argument required"))?;
    let mode = if args.offline {
        Mode::Offline
    } else if args.refresh {
        Mode::Refresh
    } else {
        Mode::Cached
    };
    let article = cached_article(&args.cache, &title, mode, fetch_page)?;
    if mode != Mode::Offline && search::Index::exists(&args.cache) {
        search::Index::open(&args.cache)?.sync(&args.cache)?;
    }
    let output = if args.list {
        article.catalog()
    } else {
        article.passages(args.section.as_deref())
    };
    write_json(&mut writer, &output)
}

fn write_json(mut writer: impl Write, output: &Value) -> Result<()> {
    serde_json::to_writer(&mut writer, output)?;
    writeln!(writer)?;
    writer.flush()?;
    Ok(())
}

fn main() {
    if let Err(error) = run(Args::parse()) {
        eprintln!("n20-wiki: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn page() -> Page {
        Page {
            title: "Hopper".to_owned(), revid: 123,
            text: r#"<div class="mw-parser-output"><p>Useful passage.</p><script>Raw HTML.</script></div>"#.to_owned(),
        }
    }

    #[test]
    fn caches_only_parsed_data_and_preserves_it_after_a_failed_refresh() {
        let dir = tempfile::tempdir().unwrap();
        let unavailable = |_: &str| -> Result<Page> { panic!("network fetch on cache hit") };
        assert!(cached_article(dir.path(), "Hopper", Mode::Offline, unavailable).is_err());
        let initial = cached_article(dir.path(), "Hopper", Mode::Cached, |_| Ok(page())).unwrap();
        let entry = fs::read_dir(dir.path())
            .unwrap()
            .map(|x| x.unwrap().path())
            .find(|path| path.extension().is_some_and(|ext| ext == "json"))
            .unwrap();
        let data = fs::read_to_string(&entry).unwrap();
        assert!(data.contains("Useful passage."));
        assert!(!data.contains("Raw HTML."));
        assert!(
            cached_article(dir.path(), "Hopper", Mode::Refresh, |_| Err(
                "unavailable".into()
            ))
            .is_err()
        );
        assert_eq!(fs::read_to_string(&entry).unwrap(), data);
        assert_eq!(
            cached_article(dir.path(), "Hopper", Mode::Cached, unavailable).unwrap(),
            initial
        );
        assert_eq!(
            cached_article(dir.path(), "Hopper", Mode::Offline, unavailable).unwrap(),
            initial
        );
    }

    #[test]
    fn recovers_a_corrupt_entry_and_accepts_normalized_titles() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(normalize_title(" Iron__Golem ").unwrap(), "Iron Golem");
        assert!(normalize_title("Hopper#Usage").is_err());
        let path = dir
            .path()
            .join(format!("{:x}.json", Sha256::digest(b"Hopper")));
        fs::write(&path, "broken").unwrap();
        let article = cached_article(dir.path(), "Hopper", Mode::Cached, |_| Ok(page())).unwrap();
        assert_eq!(read_cache(&path, "Hopper").unwrap(), article);
    }

    #[test]
    fn concurrent_misses_share_one_fetch() {
        use std::sync::{
            Barrier,
            atomic::{AtomicUsize, Ordering},
        };

        let dir = tempfile::tempdir().unwrap();
        let start = Barrier::new(2);
        let fetches = AtomicUsize::new(0);
        std::thread::scope(|scope| {
            let requests: Vec<_> = (0..2)
                .map(|_| {
                    scope.spawn(|| {
                        start.wait();
                        cached_article(dir.path(), "Hopper", Mode::Cached, |_| {
                            fetches.fetch_add(1, Ordering::Relaxed);
                            Ok(page())
                        })
                        .unwrap()
                    })
                })
                .collect();
            for request in requests {
                assert_eq!(request.join().unwrap().revision, 123);
            }
        });
        assert_eq!(fetches.load(Ordering::Relaxed), 1);
    }
}
