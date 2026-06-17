const state = {
  snapshots: [],
  trades: [],
  source: null,
};

function parseJsonl(text) {
  return text
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => JSON.parse(line));
}

async function loadFile(input, key) {
  const file = input.files[0];
  if (!file) return;
  const text = await file.text();
  state[key] = parseJsonl(text);
  render();
}

function latestSnapshot() {
  for (let i = state.snapshots.length - 1; i >= 0; --i) {
    if (state.snapshots[i].type === "snapshot") return state.snapshots[i];
  }
  return { sequence: 0, bids: [], asks: [] };
}

function renderLadder(id, levels) {
  const root = document.getElementById(id);
  root.innerHTML = "";
  const maxQty = Math.max(1, ...levels.map((level) => level.quantity));
  for (const level of levels) {
    const row = document.createElement("div");
    row.className = "row";
    const price = document.createElement("strong");
    price.textContent = level.price;
    const bar = document.createElement("div");
    bar.className = "bar";
    bar.style.width = `${Math.max(4, (level.quantity / maxQty) * 100)}%`;
    const qty = document.createElement("span");
    qty.textContent = `${level.quantity} / ${level.orders}`;
    row.append(price, bar, qty);
    root.append(row);
  }
}

function renderTrades() {
  const root = document.getElementById("tradeList");
  root.innerHTML = "";
  const trades = state.trades.filter((row) => row.type === "trade").slice(-24).reverse();
  for (const trade of trades) {
    const item = document.createElement("div");
    item.className = "trade";
    item.innerHTML = `<strong>${trade.price}</strong><span>${trade.quantity}</span><span>#${trade.sequence}</span>`;
    root.append(item);
  }
}

function render() {
  const snap = latestSnapshot();
  const bestBid = snap.bids[0]?.price;
  const bestAsk = snap.asks[0]?.price;
  const bidQty = snap.bids.reduce((sum, level) => sum + level.quantity, 0);
  const askQty = snap.asks.reduce((sum, level) => sum + level.quantity, 0);
  const total = bidQty + askQty;

  document.getElementById("sequence").textContent = snap.sequence ?? 0;
  document.getElementById("spread").textContent = bestBid && bestAsk ? bestAsk - bestBid : "-";
  document.getElementById("mid").textContent = bestBid && bestAsk ? ((bestBid + bestAsk) / 2).toFixed(1) : "-";
  document.getElementById("imbalance").textContent = total ? ((bidQty - askQty) / total).toFixed(3) : "0.000";
  renderLadder("bids", snap.bids ?? []);
  renderLadder("asks", snap.asks ?? []);
  renderTrades();
}

function connectSse() {
  if (state.source) {
    state.source.close();
  }
  const url = document.getElementById("sseUrl").value;
  state.snapshots = [];
  state.trades = [];
  state.source = new EventSource(url);
  state.source.onmessage = (event) => {
    const row = JSON.parse(event.data);
    if (row.type === "snapshot" || row.type === "metrics") {
      state.snapshots.push(row);
    }
    if (row.type === "trade") {
      state.trades.push(row);
    }
    render();
  };
}

document.getElementById("snapshots").addEventListener("change", (event) => loadFile(event.target, "snapshots"));
document.getElementById("trades").addEventListener("change", (event) => loadFile(event.target, "trades"));
document.getElementById("connect").addEventListener("click", connectSse);
render();
