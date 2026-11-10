use regex::Regex;
use scraper::{ElementRef, Html, Node, Selector};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::sync::LazyLock;

const MAX_DEPTH: usize = 96;
const BADGE: char = '\0';
const OMIT: &[&str] = &[
    "external links",
    "gallery",
    "navigation",
    "references",
    "renders",
    "screenshots",
    "see also",
];
const ON_REQUEST: &[&str] = &[
    "achievements",
    "advancements",
    "data history",
    "data values",
    "development",
    "history",
    "issues",
    "notes",
    "sounds",
    "trivia",
    "videos",
];
const SKIP_CLASS: &[&str] = &[
    "gallery",
    "hatnote",
    "metadata",
    "msgbox",
    "mw-editsection",
    "mw-empty-elt",
    "mw-references-wrap",
    "navbox",
    "navigation-not-searchable",
    "noexcerpt",
    "noprint",
    "reference",
    "references",
    "sidebar",
    "toc",
];

static OTHER_EDITION: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
    r"(?i:\b(?:Bedrock Edition|Pocket Edition|Legacy Console Edition|Minecraft Education|(?:New Nintendo|PlayStation|Xbox|Wii)\w*(?: \w+)* Edition)\b)|\b(?:BE|PE|LCE)\b"
).unwrap()
});
static EDITION_PREFIX: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
    r"(?i)^(?:In )?(Java Edition|Bedrock Edition|Pocket Edition|Legacy Console Edition|Minecraft Education)[,:]"
).unwrap()
});
static EDITION_VALUE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"\b(Java Edition|JE|Bedrock Edition|BE)\s*:").unwrap());
static JAVA_COMPARISON: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i),?\s+but (?:it|they) (?:can|cannot|do|does|is|are) in Bedrock Edition[.!?]?$")
        .unwrap()
});
static EXCLUSIVE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)is exclusive to (?:Bedrock Edition|Pocket Edition|Minecraft Education)")
        .unwrap()
});
static LIST_BADGE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r",\s*[^,.;:\n\x00]+?\x00\s*,").unwrap());
static AFTER_PUNCT: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"([.!?])[ \u{200b}\u{200c}]*\x00").unwrap());
static SENTENCE_END: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"([.!?])[\t ]+").unwrap());
static BODY: LazyLock<Selector> = LazyLock::new(|| Selector::parse(".mw-parser-output").unwrap());
static BADGE_TITLES: LazyLock<Selector> = LazyLock::new(|| Selector::parse("[title]").unwrap());
static INFO_ROWS: LazyLock<Selector> =
    LazyLock::new(|| Selector::parse("table.infobox-rows").unwrap());
static HEADLINE: LazyLock<Selector> =
    LazyLock::new(|| Selector::parse(".mw-headline[id]").unwrap());
static CRAFT_INPUT: LazyLock<Selector> =
    LazyLock::new(|| Selector::parse(".mcui-Crafting_Table .mcui-input").unwrap());
static SLOTS: LazyLock<Selector> = LazyLock::new(|| Selector::parse(".invslot").unwrap());
static SLOT_ITEM: LazyLock<Selector> = LazyLock::new(|| Selector::parse(".invslot-item").unwrap());
static SLOT_LINK: LazyLock<Selector> =
    LazyLock::new(|| Selector::parse(".invslot-item a[title][href^='/w/']").unwrap());
static SLOT_DECORATION: LazyLock<Selector> =
    LazyLock::new(|| Selector::parse("img, .invslot-stacksize").unwrap());

#[derive(Deserialize)]
pub struct Page {
    pub title: String,
    pub revid: u64,
    pub text: String,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct Article {
    pub(crate) title: String,
    pub revision: u64,
    pub(crate) sections: Vec<Section>,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub(crate) struct Section {
    pub(crate) heading: String,
    pub(crate) anchor: String,
    path: Vec<String>,
    pub(crate) default: bool,
    pub(crate) blocks: Vec<Block>,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub(crate) enum Block {
    Text { text: String },
    List { text: String, depth: usize },
    Row { text: String },
}

impl Article {
    pub fn passages(&self, anchor: Option<&str>) -> Value {
        let sections: Vec<_> = self
            .sections
            .iter()
            .filter(|section| {
                !section.blocks.is_empty()
                    && match anchor {
                        Some(anchor) => {
                            section.anchor == anchor
                                || section.path.iter().any(|part| part == anchor)
                        }
                        None => section.default,
                    }
            })
            .collect();
        json!({"title": self.title, "revision": self.revision, "sections": sections})
    }

    pub fn catalog(&self) -> Value {
        let populated: std::collections::HashSet<_> = self
            .sections
            .iter()
            .filter(|section| !section.blocks.is_empty())
            .flat_map(|section| section.path.iter().map(String::as_str))
            .collect();
        let sections: Vec<_> = self
            .sections
            .iter()
            .filter(|section| {
                !section.blocks.is_empty() || populated.contains(section.anchor.as_str())
            })
            .map(|section| {
                json!({"heading": section.heading, "anchor": section.anchor,
                "path": section.path, "default": section.default, "blocks": section.blocks.len()})
            })
            .collect();
        json!({"title": self.title, "revision": self.revision, "sections": sections})
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Edition {
    Java,
    Other,
}

fn edition_label(text: &str) -> Option<Edition> {
    let text = text.trim_end_matches(':').trim();
    match text.to_ascii_lowercase().as_str() {
        "java edition" | "je" => Some(Edition::Java),
        "be" | "pe" | "lce" | "minecraft education" => Some(Edition::Other),
        _ if text.ends_with(" Edition") && OTHER_EDITION.is_match(text) => Some(Edition::Other),
        _ => None,
    }
}

fn excluded(node: ElementRef<'_>) -> bool {
    matches!(
        node.value().name(),
        "aside"
            | "audio"
            | "button"
            | "figcaption"
            | "figure"
            | "footer"
            | "form"
            | "header"
            | "nav"
            | "script"
            | "style"
            | "svg"
            | "video"
    ) || node.attr("hidden").is_some()
        || node
            .value()
            .classes()
            .any(|class| SKIP_CLASS.contains(&class))
}

fn plain_children(node: ElementRef<'_>, depth: usize, output: &mut String) {
    if depth >= MAX_DEPTH {
        return;
    }
    for child in node.children() {
        match child.value() {
            Node::Text(text) => output.extend(text.chars().filter(|c| *c != BADGE)),
            Node::Element(_) => append_plain(ElementRef::wrap(child).unwrap(), depth + 1, output),
            _ => (),
        }
    }
}

fn append_plain(node: ElementRef<'_>, depth: usize, output: &mut String) {
    let tag = node.value().name();
    if tag == "sup"
        && node
            .value()
            .classes()
            .any(|class| matches!(class, "Inline-Template" | "reference"))
    {
        if node
            .select(&BADGE_TITLES)
            .filter_map(|child| child.attr("title"))
            .any(|title| {
                OTHER_EDITION.is_match(title) || title.contains("only to upcoming versions")
            })
        {
            output.push(BADGE);
        }
        return;
    }
    if excluded(node) || matches!(tag, "dl" | "img" | "ol" | "table" | "ul") {
        return;
    }
    if tag == "br" {
        output.push('\n');
        return;
    }
    let block = matches!(tag, "div" | "li" | "p");
    if block {
        output.push('\n');
    }
    plain_children(node, depth, output);
    if block {
        output.push('\n');
    }
}

fn plain(node: ElementRef<'_>) -> String {
    let mut output = String::new();
    append_plain(node, 0, &mut output);
    output
}

fn compact(text: &str) -> String {
    text.replace(['\u{200b}', '\u{200c}'], "")
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
}

fn java_text(text: &str) -> String {
    let list = LIST_BADGE.replace_all(text, ",");
    let markers = AFTER_PUNCT.replace_all(&list, "\0$1");
    let sentences = SENTENCE_END.replace_all(&markers, "$1\n");
    let mut scope = None;
    let mut kept = Vec::new();
    for sentence in sentences.lines() {
        let mut part = compact(sentence);
        let prefix = EDITION_PREFIX
            .captures(&part)
            .and_then(|c| edition_label(&c[1]));
        if prefix.is_some() {
            scope = prefix;
        } else if part.starts_with("In both editions,") || part.starts_with("In both versions,") {
            scope = None;
        }
        if part.contains(BADGE) || scope == Some(Edition::Other) {
            continue;
        }
        if prefix == Some(Edition::Java) {
            part = JAVA_COMPARISON.replace(&part, ".").into_owned();
        }
        if !part.is_empty() && !OTHER_EDITION.is_match(&part) {
            kept.push(part);
        }
    }
    kept.join(" ")
}

fn java_value(text: &str) -> String {
    let mut labels = EDITION_VALUE.captures_iter(text).peekable();
    if labels.peek().is_none() {
        return java_text(text);
    }
    while let Some(label) = labels.next() {
        if edition_label(&label[1]) == Some(Edition::Java) {
            let end = labels
                .peek()
                .map_or(text.len(), |next| next.get(0).unwrap().start());
            return java_text(&text[label.get(0).unwrap().end()..end]);
        }
    }
    String::new()
}

fn history(rows: &[ElementRef<'_>]) -> Vec<String> {
    let mut result = Vec::new();
    let mut version = String::new();
    let mut remaining: usize = 0;
    for row in rows {
        let headings: Vec<_> = row
            .child_elements()
            .filter(|cell| cell.value().name() == "th")
            .collect();
        let labels: Vec<_> = headings.iter().map(|cell| compact(&plain(*cell))).collect();
        if labels.len() == 1 {
            match edition_label(&labels[0]) {
                Some(Edition::Other) => break,
                Some(Edition::Java) => continue,
                None => (),
            }
        }
        let inherited = if remaining > 0 {
            version.clone()
        } else {
            String::new()
        };
        remaining = remaining.saturating_sub(1);
        let changes: Vec<_> = row
            .child_elements()
            .filter(|cell| cell.value().name() == "td")
            .map(|cell| java_text(&plain(cell)))
            .filter(|text| !text.is_empty())
            .collect();
        if changes.is_empty()
            || labels
                .iter()
                .any(|s| s.contains(BADGE) || OTHER_EDITION.is_match(s))
        {
            continue;
        }
        if let Some(span) = headings
            .first()
            .and_then(|cell| cell.attr("rowspan"))
            .and_then(|value| value.parse::<usize>().ok())
        {
            version.clone_from(&labels[0]);
            remaining = span.saturating_sub(1);
        }
        result.push(
            std::iter::once(inherited)
                .chain(labels)
                .chain(changes)
                .filter(|text| !text.is_empty())
                .collect::<Vec<_>>()
                .join(" "),
        );
    }
    result
}

fn crafting_ingredients(cell: ElementRef<'_>) -> Option<String> {
    let mut inputs = cell.select(&CRAFT_INPUT);
    let input = inputs.next()?;
    if inputs.next().is_some() {
        return None;
    }
    let slots: Vec<_> = input.select(&SLOTS).collect();
    if !matches!(slots.len(), 4 | 9) {
        return None;
    }
    let mut totals = Vec::<(String, usize)>::new();
    let mut alternatives = std::collections::HashSet::new();
    for slot in slots {
        let items = slot.select(&SLOT_ITEM).count();
        if items == 0 {
            if slot.select(&SLOT_DECORATION).next().is_some() {
                return None;
            }
            continue;
        }
        let names: Vec<_> = slot
            .select(&SLOT_LINK)
            .filter_map(|link| link.attr("title"))
            .map(compact)
            .collect();
        if names.len() != items
            || slot
                .select(&SLOT_DECORATION)
                .any(|node| node.value().name() != "img")
            || names.iter().any(|name| {
                name.is_empty()
                    || name.len() > 100
                    || name.starts_with("File:")
                    || OTHER_EDITION.is_match(name)
            })
        {
            return None;
        }
        let mut names = names.into_iter();
        let name = names.next()?;
        if items > 1 {
            alternatives.extend(std::iter::once(name).chain(names));
            continue;
        }
        if let Some((_, count)) = totals.iter_mut().find(|(item, _)| *item == name) {
            *count += 1;
        } else {
            totals.push((name, 1));
        }
    }
    // A fixed slot only establishes an exact total if no variable slot could add
    // another copy of that same item.
    totals.retain(|(name, _)| !alternatives.contains(name));
    (!totals.is_empty()).then(|| {
        let mut summary = totals
            .into_iter()
            .map(|(name, count)| format!("{count} × {name}"))
            .collect::<Vec<_>>()
            .join(", ");
        if !alternatives.is_empty() {
            summary.push_str(" (fixed ingredient counts; other slots vary)");
        }
        summary
    })
}

fn table(node: ElementRef<'_>) -> Vec<String> {
    let infobox = node
        .value()
        .has_class("infobox-rows", scraper::CaseSensitivity::CaseSensitive);
    if !infobox
        && !node
            .value()
            .has_class("wikitable", scraper::CaseSensitivity::CaseSensitive)
    {
        return Vec::new();
    }
    let rows: Vec<_> = node
        .child_elements()
        .flat_map(|child| {
            if child.value().name() == "tr" {
                vec![child]
            } else if matches!(child.value().name(), "thead" | "tbody" | "tfoot") {
                child
                    .child_elements()
                    .filter(|row| row.value().name() == "tr")
                    .collect()
            } else {
                Vec::new()
            }
        })
        .collect();
    if node.attr("data-description") == Some("History") {
        return history(&rows);
    }
    let caption = node
        .child_elements()
        .find(|child| child.value().name() == "caption")
        .map(plain)
        .unwrap_or_default();
    if edition_label(&compact(&caption)) == Some(Edition::Other) {
        return Vec::new();
    }
    let caption = java_text(&caption);
    let mut headers = Vec::new();
    let mut scope = None;
    let mut result = Vec::new();
    for row in rows {
        let cells: Vec<_> = row
            .child_elements()
            .filter(|cell| matches!(cell.value().name(), "th" | "td"))
            .collect();
        if cells.len() == 1
            && cells[0].value().name() == "th"
            && let Some(label) = edition_label(&compact(&plain(cells[0])))
        {
            scope = Some(label);
            headers.clear();
            continue;
        }
        if scope == Some(Edition::Other)
            || cells.is_empty()
            || cells.iter().any(|cell| {
                cell.attr("colspan").unwrap_or("1") != "1"
                    || cell.attr("rowspan").unwrap_or("1") != "1"
            })
        {
            continue;
        }
        if cells.iter().all(|cell| cell.value().name() == "th") {
            headers = cells.iter().map(|cell| compact(&plain(*cell))).collect();
            continue;
        }
        let raw: Vec<_> = cells.iter().map(|cell| plain(*cell)).collect();
        let mut values: Vec<_> = raw.iter().map(|text| java_value(text)).collect();
        // A cutting widget may combine several animated result counts in one cell.
        // The unlabeled digits cannot be attributed to a particular output.
        if node.attr("data-description") == Some("Cutting recipes")
            && headers.len() == cells.len()
            && let Some(index) = headers.iter().position(|label| label == "Cutting recipe")
        {
            values[index].clear();
        }
        if node.attr("data-description") == Some("Crafting recipes")
            && headers.len() == cells.len()
            && !raw
                .iter()
                .any(|text| text.contains(BADGE) || OTHER_EDITION.is_match(text))
            && let Some(index) = headers.iter().position(|label| label == "Crafting recipe")
            && cells[index].select(&CRAFT_INPUT).next().is_some()
        {
            if let Some(ingredients) = crafting_ingredients(cells[index]) {
                values[index] = ingredients;
            } else if values[index]
                .bytes()
                .all(|ch| ch.is_ascii_digit() || ch.is_ascii_whitespace())
            {
                // Animated output counts are not ingredient counts.
                values[index].clear();
            }
        }
        if infobox && values.len() == 2 && values.iter().all(|s| !s.is_empty()) {
            result.push(format!("{}: {}", values[0], values[1]));
        } else if headers.len() == values.len()
            && !headers.iter().any(|label| {
                label.contains(BADGE)
                    || OTHER_EDITION.is_match(label) && edition_label(label).is_none()
            })
        {
            let labeled: Vec<_> = headers
                .iter()
                .zip(values)
                .filter(|(label, value)| {
                    !label.is_empty()
                        && !value.is_empty()
                        && edition_label(label) != Some(Edition::Other)
                })
                .map(|(label, value)| format!("{label}: {value}"))
                .collect();
            if !labeled.is_empty() {
                result.push(
                    std::iter::once(caption.as_str())
                        .chain(labeled.iter().map(String::as_str))
                        .filter(|text| !text.is_empty())
                        .collect::<Vec<_>>()
                        .join("; "),
                );
            }
        }
    }
    result
}

struct Heading {
    level: u8,
    anchor: String,
    omit: bool,
    on_request: bool,
}

struct Extractor {
    sections: Vec<Section>,
    headings: Vec<Heading>,
    current: Option<usize>,
    label: Option<Edition>,
    exclusive: bool,
}

impl Extractor {
    fn heading(&mut self, node: ElementRef<'_>, level: u8) {
        while self
            .headings
            .last()
            .is_some_and(|heading| heading.level >= level)
        {
            self.headings.pop();
        }
        let heading = compact(&plain(node));
        let anchor = node
            .attr("id")
            .or_else(|| {
                node.select(&HEADLINE)
                    .next()
                    .and_then(|child| child.attr("id"))
            })
            .unwrap_or_default()
            .to_owned();
        let lower = heading.to_ascii_lowercase();
        self.headings.push(Heading {
            level,
            anchor: anchor.clone(),
            omit: OMIT.contains(&lower.as_str()) || edition_label(&heading) == Some(Edition::Other),
            on_request: ON_REQUEST.contains(&lower.as_str()),
        });
        self.label = None;
        self.current = None;
        if self.headings.iter().any(|heading| heading.omit) {
            return;
        }
        self.current = Some(self.sections.len());
        self.sections.push(Section {
            heading,
            anchor,
            path: self
                .headings
                .iter()
                .filter(|heading| !heading.anchor.is_empty())
                .map(|heading| heading.anchor.clone())
                .collect(),
            default: !self.headings.iter().any(|heading| heading.on_request),
            blocks: Vec::new(),
        });
    }

    fn visit(&mut self, node: ElementRef<'_>, depth: usize, list_depth: usize) {
        if self.exclusive || depth >= MAX_DEPTH {
            return;
        }
        if node
            .value()
            .has_class("msgbox", scraper::CaseSensitivity::CaseSensitive)
        {
            let mut content = String::new();
            plain_children(node, 0, &mut content);
            if EXCLUSIVE.is_match(&compact(&content)) {
                if let Some(heading) = self.headings.last_mut() {
                    heading.omit = true;
                    if let Some(index) = self.current.take() {
                        self.sections.truncate(index);
                    }
                } else {
                    self.exclusive = true;
                    self.sections.clear();
                }
            }
            return;
        }
        if excluded(node) {
            return;
        }
        let tag = node.value().name();
        if let [b'h', level @ b'1'..=b'6'] = tag.as_bytes() {
            self.heading(node, *level - b'0');
            return;
        }
        if tag == "p" {
            let label = compact(&plain(node));
            if label.ends_with(':')
                && let Some(edition) = edition_label(&label)
            {
                self.label = Some(edition);
                return;
            }
        }
        let active = self.current.filter(|_| self.label != Some(Edition::Other));
        if node
            .value()
            .has_class("infobox", scraper::CaseSensitivity::CaseSensitive)
        {
            if let Some(index) = active {
                for info in node.select(&INFO_ROWS) {
                    self.sections[index]
                        .blocks
                        .extend(table(info).into_iter().map(|text| Block::Row { text }));
                }
            }
            return;
        }
        if tag == "table" {
            if let Some(index) = active {
                self.sections[index]
                    .blocks
                    .extend(table(node).into_iter().map(|text| Block::Row { text }));
            }
            return;
        }
        if matches!(tag, "p" | "li" | "dt" | "dd" | "pre") {
            if let Some(index) = active {
                let text = java_text(&plain(node));
                if !text.is_empty() {
                    self.sections[index].blocks.push(if tag == "li" {
                        Block::List {
                            text,
                            depth: list_depth,
                        }
                    } else {
                        Block::Text { text }
                    });
                    if tag == "li" {
                        for child in node
                            .child_elements()
                            .filter(|el| matches!(el.value().name(), "ul" | "ol"))
                        {
                            self.visit(child, depth + 1, list_depth + 1);
                        }
                    }
                }
            }
            return;
        }
        for child in node.child_elements() {
            self.visit(child, depth + 1, list_depth);
        }
    }
}

pub fn extract(page: Page) -> Result<Article, &'static str> {
    if page.revid == 0 || page.title.is_empty() {
        return Err("invalid article title or revision");
    }
    if ["bedrock edition", "pocket edition", "minecraft education"]
        .iter()
        .any(|edition| page.title.to_ascii_lowercase().starts_with(edition))
    {
        return Ok(Article {
            title: page.title,
            revision: page.revid,
            sections: Vec::new(),
        });
    }
    let html = Html::parse_fragment(&page.text);
    let body = html
        .select(&BODY)
        .next()
        .ok_or("MediaWiki article body not found")?;
    let mut extractor = Extractor {
        sections: vec![Section {
            heading: "Overview".into(),
            anchor: String::new(),
            path: Vec::new(),
            default: true,
            blocks: Vec::new(),
        }],
        headings: Vec::new(),
        current: Some(0),
        label: None,
        exclusive: false,
    };
    extractor.visit(body, 0, 0);
    Ok(Article {
        title: page.title,
        revision: page.revid,
        sections: extractor.sections,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_labeled_crafting_slots_and_leaves_alternatives_unresolved() {
        let slots = r#"
            <span class="invslot"><span class="invslot-item"><a href="/w/Copper_Ingot" title="Copper Ingot"><img src="sprite.png"></a></span></span>
            <span class="invslot"><span class="invslot-item"><a href="/w/Copper_Ingot" title="Copper Ingot"></a></span></span>
            <span class="invslot"><span class="invslot-item"><a href="/w/Copper_Ingot" title="Copper Ingot"></a></span></span>
            <span class="invslot"></span>"#;
        let row = |contents: &str| {
            format!(
                r#"<tr><td>Copper Ingot</td><td>
            <span class="mcui-Crafting_Table"><span class="mcui-input">{contents}</span></span>
            </td></tr>"#
            )
        };
        let alternative = slots.replacen(
            "</a></span>",
            r#"</a><a href="/w/Iron_Ingot" title="Iron Ingot"></a></span>"#,
            1,
        );
        let page = article(&format!(
            r#"<h2 id="Crafting">Crafting</h2>
            <table class="wikitable" data-description="Crafting recipes">
            <tr><th>Ingredients</th><th>Crafting recipe</th></tr>{}{}</table>"#,
            row(slots),
            row(&alternative)
        ));
        let text = page.passages(Some("Crafting")).to_string();
        assert_eq!(text.matches("Crafting recipe: 3 × Copper Ingot").count(), 1);
        assert!(!text.contains("Iron Ingot") && !text.contains("sprite.png"));
    }

    #[test]
    fn keeps_only_counts_unaffected_by_animated_slots() {
        let item = |name| {
            format!(r#"<span class="invslot-item"><a href="/w/Item" title="{name}"></a></span>"#)
        };
        let slot = |items| format!(r#"<span class="invslot">{items}</span>"#);
        let fixed = slot(item("Book"));
        let optional = slot(item("Oak Planks") + &item("Spruce Planks"));
        let includes_fixed = slot(item("Book") + &item("Oak Planks"));
        let row = |variant| {
            format!(
                r#"<tr><td>Book and planks</td><td><span class="mcui-Crafting_Table"><span class="mcui-input">{fixed}{fixed}{variant}<span class="invslot"></span></span>333</span></td></tr>"#,
            )
        };
        let page = article(&format!(
            r#"<table class="wikitable" data-description="Crafting recipes"><tr><th>Ingredients</th><th>Crafting recipe</th></tr>{}{}</table>"#,
            row(optional),
            row(includes_fixed)
        ));
        let text = page.passages(None).to_string();
        assert_eq!(
            text.matches("2 × Book (fixed ingredient counts; other slots vary)")
                .count(),
            1
        );
        assert!(!text.contains("333"));
    }

    fn article(body: &str) -> Article {
        extract(Page {
            title: "Example".into(),
            revid: 123,
            text: format!(
                r#"<nav><p>Outside.</p></nav><div class="mw-parser-output">{body}</div>"#
            ),
        })
        .unwrap()
    }

    #[test]
    fn retains_java_rows_and_excludes_marked_sections() {
        let page = article(
            r#"
            <div class="infobox"><p>Bedrock Edition:</p>
              <table class="infobox-rows"><tr><th>Waterloggable</th><td><b>JE:</b> No<br><b>BE:</b> Yes</td></tr></table></div>
            <p>Bedrock is a block.</p><figure><figcaption>Media noise.</figcaption></figure>
            <p>Loot comes from chests, fishing, raids<sup class="Inline-Template"><span title="This statement only applies to Bedrock Edition">BE</span></sup>, or trading.</p>
            <p>Exclusive.<sup class="Inline-Template"><span title="This statement only applies to Bedrock Edition">BE</span></sup> Shared fact.</p>
            <p>In Java Edition, potions are treated alike, but they are in Bedrock Edition.</p>
            <h2 id="Path">Preferred path</h2><div class="msgbox">This feature is exclusive to Bedrock Edition.</div>
            <p>Wrong path.</p><h3 id="Child">Child</h3><p>Wrong descendant.</p>
            <h2 id="Usage">Usage</h2><ol><li>Place the block.<ul><li>Use stone.</li></ul></li></ol>
            <table class="wikitable"><caption>A single beacon pyramid</caption><tr><th>Level</th><th>Mineral blocks</th></tr>
              <tr><td>4</td><td>164</td></tr></table>
            <h2 id="Gallery">Gallery</h2><p>Gallery noise.</p>"#,
        );
        let text = page.passages(None).to_string();
        for expected in [
            "Waterloggable: No",
            "Bedrock is a block.",
            "Loot comes from chests, fishing, or trading.",
            "Shared fact.",
            "In Java Edition, potions are treated alike.",
            "Use stone.",
            "A single beacon pyramid; Level: 4; Mineral blocks: 164",
        ] {
            assert!(text.contains(expected), "{expected}: {text}");
        }
        for excluded in [
            "Bedrock Edition",
            "Media noise",
            "Outside",
            "raids",
            "Exclusive.",
            "Wrong",
            "Gallery noise",
        ] {
            assert!(!text.contains(excluded), "{excluded}: {text}");
        }
    }

    #[test]
    fn optional_sections_use_stable_anchors_and_keep_edition_scope() {
        let page = article(
            r#"<p>Default.</p><h2 id="Sounds">Sounds</h2>
            <p>Java Edition:</p><table class="wikitable"><tr><th>Identifier</th></tr><tr><td>block.metal.hit</td></tr></table>
            <p>Bedrock Edition:</p><table class="wikitable"><tr><th>Identifier</th></tr><tr><td>bedrock.only</td></tr></table>
            <h2 id="History">History</h2><h3 id="Java_Edition">Java Edition</h3>
            <table class="wikitable" data-description="History"><tr><th colspan="8">Java Edition</th></tr>
              <tr><th rowspan="2" colspan="3">1.9</th><th colspan="4">15w42a</th><td>Added an item.</td></tr>
              <tr><th colspan="4">15w43a</th><td>Updated the item.</td></tr></table>"#,
        );
        assert_eq!(page.passages(None)["sections"].as_array().unwrap().len(), 1);
        assert!(
            page.passages(Some("Sounds"))
                .to_string()
                .contains("Identifier: block.metal.hit")
        );
        assert!(
            !page
                .passages(Some("Sounds"))
                .to_string()
                .contains("bedrock.only")
        );
        assert!(
            page.passages(Some("History"))
                .to_string()
                .contains("1.9 15w43a Updated the item.")
        );
        assert!(page.catalog().to_string().contains("Java_Edition"));
        assert!(!page.catalog().to_string().contains("Updated the item."));
    }
}
