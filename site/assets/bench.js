/* Renders data/benchmarks.json into #bench-app.
   The page already contains a static copy of the same tables, so without JavaScript
   (or when the fetch fails, e.g. from file://) nothing is lost. */
(function () {
  "use strict";
  var app = document.getElementById("bench-app");
  if (!app || !window.fetch || !document.createDocumentFragment) return;

  function el(tag, attrs, text) {
    var node = document.createElement(tag);
    if (attrs) {
      for (var key in attrs) {
        if (Object.prototype.hasOwnProperty.call(attrs, key)) node.setAttribute(key, attrs[key]);
      }
    }
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function fps(value, approximate) {
    if (typeof value !== "number") return "n/a";
    return (approximate ? "~" : "") + String(value);
  }

  function td(label, text, cls) {
    var cell = el("td", { "data-label": label }, text);
    if (cls) cell.className = cls;
    return cell;
  }

  function renderTable(group, rows) {
    var section = el("section", { "class": "bench-group", "aria-labelledby": "group-" + group.id });
    section.appendChild(el("h2", { id: "group-" + group.id }, group.title));
    if (group.description) section.appendChild(el("p", { "class": "section-intro" }, group.description));

    var wrap = el("div", { "class": "table-wrap" });
    var table = el("table", { "class": "stack" });
    table.appendChild(el("caption", { "class": "visually-hidden" }, group.title));
    var thead = el("thead");
    var head = el("tr");
    var cols = ["Machine", "Driver", "Game and scene", "Build or change", "Frame generation", "Game fps", "Notes"];
    cols.forEach(function (name) {
      var th = el("th", { scope: "col" }, name);
      if (name === "Game fps") th.className = "num";
      head.appendChild(th);
    });
    thead.appendChild(head);
    table.appendChild(thead);

    var body = el("tbody");
    rows.forEach(function (row) {
      var tr = el("tr", { id: row.id });
      var machine = row.machine || {};
      tr.appendChild(td("Machine", [machine.cpu, machine.gpu].filter(Boolean).join(" · ")));
      tr.appendChild(td("Driver", machine.driver || "n/a"));
      tr.appendChild(td("Game and scene", [row.game, row.scene].filter(Boolean).join(", ")));
      tr.appendChild(td("Build or change", row.change || row.build || "n/a"));
      tr.appendChild(td("Frame generation", row.frame_generation || "n/a"));
      var result = row.result || {};
      var value = fps(result.game_fps, result.approximate);
      if (row.baseline && typeof row.baseline.game_fps === "number") {
        value = fps(row.baseline.game_fps, false) + " → " + value;
      }
      tr.appendChild(td("Game fps", value, "num"));
      var notes = [];
      (row.extra || []).forEach(function (x) {
        if (typeof x.baseline === "number") notes.push(x.label + ": " + x.baseline + " → " + x.value + " " + x.unit + ".");
        else notes.push(x.label + ": " + x.value + " " + x.unit + ".");
      });
      if (row.caveat) notes.push(row.caveat);
      tr.appendChild(td("Notes", notes.join(" ")));
      body.appendChild(tr);
    });
    table.appendChild(body);
    wrap.appendChild(table);
    section.appendChild(wrap);
    return section;
  }

  fetch(app.getAttribute("data-src"), { cache: "no-cache" })
    .then(function (response) {
      if (!response.ok) throw new Error("HTTP " + response.status);
      return response.json();
    })
    .then(function (data) {
      var groups = data.groups || [];
      var rows = data.rows || [];
      if (!groups.length || !rows.length) return;
      var frag = document.createDocumentFragment();
      groups.forEach(function (group) {
        var mine = rows.filter(function (row) { return row.group === group.id; });
        if (mine.length) frag.appendChild(renderTable(group, mine));
      });
      var stamp = el("p", { "class": "fine" });
      stamp.appendChild(document.createTextNode("Rendered from "));
      stamp.appendChild(el("a", { href: app.getAttribute("data-src") }, "data/benchmarks.json"));
      stamp.appendChild(document.createTextNode(data.updated ? " (updated " + data.updated + ")." : "."));
      frag.appendChild(stamp);
      while (app.firstChild) app.removeChild(app.firstChild);
      app.appendChild(frag);
      app.setAttribute("data-rendered", "client");
    })
    .catch(function () {
      /* Keep the static tables. */
    });
})();
