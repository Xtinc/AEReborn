#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const echarts = require('echarts');
const {slotTop, portMarkHeight, nodeKey, ribbonWidth, inactive, cardBox, orderFanouts, arrangeGraph, occupiedAreas,
    planFlows, placeEdgeLabel} = require('./graph_layout');

const colors = {
    ink: '#0F172A', secondary: '#475569', border: '#64748B', inactive: '#CBD5E1',
};
const ribbonPaint = {
    flow: ['#2DD4BF', '#2DD4BF', 0.5],
    ordinary: ['#60A5FA', '#60A5FA', 0.45],
    warm: ['#2DD4BF', '#FDE68A', 0.55],
    muted: ['#CBD5E1', '#CBD5E1', 0.5],
};
const margin = 16;
const headerHeight = 32;
const backendGap = 24;
const tagHeight = 15;
const value = number => Number(number.toFixed(2));
const xml = text => String(text).replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\ufffe\uffff]/gu, '\ufffd')
    .replace(/[&<>"']/g, ch => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;'})[ch]);

function text(x, y, label, attributes = '') {
    return `<text x="${value(x)}" y="${value(y)}" ${attributes}>${xml(label)}</text>`;
}

function textWidth(label, size) {
    let units = 0;
    for (const ch of label)
        units += /[ilI.:'| ]/.test(ch) ? 0.32 : /[MWmw@%]/.test(ch) ? 0.86 : ch.codePointAt(0) > 0xff ? 1 : 0.61;
    return Math.ceil(units * size);
}

const tagWidth = label => textWidth(label, 9) + 12;

function tag(x, y, label, tone) {
    const width = tagWidth(label);
    return `<g><rect class="tag ${tone}" x="${value(x)}" y="${value(y)}" width="${width}" height="${tagHeight}" rx="7.5"/>
        ${text(x + width / 2, y + 10.5, label, 'class="tag-label" text-anchor="middle"')}</g>`;
}

const rate = hz => `${hz / 1000}k`;
const specText = spec => spec?.channels ? `${rate(spec.rate)}/${spec.channels}ch/${spec.format}` : 'None';
const queueFraction = queue => queue?.capacity ? Math.max(0, Math.min(1, (queue.frames ?? 0) / queue.capacity)) : 0;
const queueWarning = queue => queueFraction(queue) >= 0.7 || !!(queue?.underrun || queue?.overflow);

function nodeDetails(node) {
    const input = node.inputs[0];
    const output = node.outputs[0];
    if (node.type === 'Convert' && input && output) {
        const changes = [];
        if (input.rate !== output.rate) {
            const suffix = input.format === output.format ? ` ${output.channels === 1 ? '1ch' : output.format}` : '';
            changes.push(`${rate(input.rate)} -> ${rate(output.rate)}${suffix}`);
        }
        if (input.format !== output.format)
            changes.push(`${input.format} -> ${output.format}`);
        return changes.length ? changes : ['passthrough'];
    }
    if (node.gain !== undefined && input && output) {
        const channelMap = input.channels !== output.channels;
        const gain = `Gain ${node.gain.toFixed(2)}${node.gain === 0 ? ' [MUTE]' : ''}`;
        return channelMap ? [`${input.channels}ch -> ${output.channels}ch`, gain] : [gain];
    }
    if (input && output && specText(input) !== specText(output))
        return [specText(input), `-> ${specText(output)}`];
    return input || output ? [specText(output || input)] : [];
}

function nodeStatus(node) {
    if (node.paused)
        return node.in_plan ? ['PAUSE PENDING', 'warn'] : ['PAUSED', 'off'];
    if (inactive(node))
        return ['OFFLINE', 'off'];
    if (node.queue) {
        if (!node.queue.online)
            return ['OFFLINE', 'off'];
        return node.queue.steady ? ['ONLINE', 'on'] : ['PREFILL', 'warn'];
    }
    if (node.gain === 0)
        return ['MUTE', 'off'];
    return null;
}

function queueDetails(queue) {
    if (!queue)
        return [];
    const percent = queue.frames === null || !queue.capacity
        ? '' : ` (${Math.round(100 * queue.frames / queue.capacity)}%)`;
    const lines = [`Queue ${queue.frames ?? '--'}/${queue.capacity}${percent}`];
    if (queue.underrun)
        lines.push(`Underrun ${queue.underrun}f`);
    if (queue.overflow)
        lines.push(`Overflow ${queue.overflow}f`);
    return lines;
}

function backendLabel(backend) {
    const name = Array.from(backend.file || '');
    const file = name.length ? `("${name.length > 30 ? name.slice(0, 27).join('') + '...' : backend.file}")` : '';
    const error = backend.error && backend.error !== 'OK' ? `ERR: ${backend.error}` : '';
    return {backend, file, error, nameWidth: textWidth(backend.type, 10), fileWidth: textWidth(file, 10),
        errorWidth: error ? tagWidth(error) : 0};
}

function makeCard(node, backend, incoming, outgoing, styles, running, position) {
    const title = node.name ? `${node.type} (${node.name})` : node.type;
    const status = nodeStatus(node);
    const details = nodeDetails(node);
    if (details.length && node.type !== 'Convert' && node.type !== 'Matrix')
        details[0] = `#${node.id} [${details[0]}]`;
    const queue = queueDetails(node.queue);
    const hub = !!node.slots || node.inputs.length > 1 || node.outputs.length > 1;
    const output = !node.inputs.length;
    const slotPorts = hub ? (output ? node.outputs : node.inputs) : [];
    const slots = slotPorts.map((_, i) => {
        if (output) {
            const linked = outgoing.some(edge => edge.from[1] === i && edge.in_plan);
            return {label: `Out ${i}${linked ? '' : ' [Empty]'}`, tone: linked ? '' : 'off-text'};
        }
        const {high, muted, suppressed} = node.slots?.[i] ?? {};
        const linked = incoming.get(i)?.in_plan;
        const flags = [];
        if (high)
            flags.push('HIGH');
        if (muted)
            flags.push('MUTE');
        else if (suppressed && !high)
            flags.push('SUPP');
        if (!linked)
            flags.push('Empty');
        const label = flags.length ? `Slot ${i} [${flags.join(' ')}]` : `Slot ${i}`;
        return {label, tone: muted ? 'off-text' : high ? 'high-text' : suppressed ? 'warn-text'
            : linked ? '' : 'off-text'};
    });
    const badges = status ? [status] : [];
    if (!status && running && node.in_plan) {
        if (hub) {
            badges.push(['ACTIVE', 'on']);
            if (node.slots?.some(slot => slot.muted))
                badges.push(['MUTE', 'off']);
            if ([...incoming.values()].some(link => link.offline))
                badges.push(['OFFLINE', 'off']);
        } else if (!node.inputs.length || !node.outputs.length) {
            badges.push(['ONLINE', 'on']);
        }
    }
    const sideWidth = backend ? backendGap + Math.max(backend.nameWidth, backend.fileWidth, backend.errorWidth) : 0;
    const badgeSpan = badges.reduce((span, [label]) => span + tagWidth(label) + 3, 0);
    const width = Math.ceil(Math.max(position?.[2] ?? (node.queue ? 112 : 91), textWidth(title, 12) + 16, badgeSpan + 12,
        ...details.map(line => textWidth(line, 10) + 16),
        ...queue.map(line => textWidth(line, 10) + 8), ...slots.map(slot => textWidth(slot.label, 10) + 16)));
    const minHeight = Math.max(position?.[3] ?? 0, node.queue ? 74 : 42, 38 + Math.max(0, details.length - 1) * 12);
    const queueHeight = queue.length ? queue.length * 13 + 5 : 0;
    const firstSlot = Math.max(slotTop, 43 + Math.max(0, details.length - 1) * 12);
    const geometry = cardBox(node, width, minHeight, sideWidth, queueHeight, hub, firstSlot, outgoing, styles);
    return {node, backend, width, title, details, badges, queue, hub, slots, incoming, ...geometry};
}

function wireStyle(edge, source, target) {
    if (!edge.in_plan || [source, target].some(node => inactive(node) || (node.queue && !node.queue.online)))
        return {width: 1.5, dash: [4, 4], color: colors.inactive, paused: true};
    const slot = target.slots?.[edge.to[1]];
    const {channels} = source.outputs[edge.from[1]];
    const width = ribbonWidth(channels);
    if (slot?.muted || source.gain === 0)
        return {width, palette: 'muted'};
    if (slot?.suppressed && !slot.high)
        return {width: 1.5, dash: [5, 4], color: '#D97706'};
    if (queueWarning(target.queue))
        return {width, palette: 'warm'};
    return {width, palette: slot?.high || channels === 1 ? 'ordinary' : 'flow'};
}

function backendTitle(backend) {
    const lines = [backend.type, backend.file, `State: ${backend.state}`, `Device: ${specText(backend.device)}`,
        `Max batch: ${backend.max_frames ?? 0} frames`, `Mode: ${backend.async ? 'async' : backend.active ? 'sync' : 'passive'}`,
        `FIR: ${backend.fir ? 'yes' : 'no'}`, `Error: ${backend.error}${backend.error_message ? ` - ${backend.error_message}` : ''}`];
    if (backend.drift_ppm !== undefined)
        lines.push(`Drift: ${backend.drift_ppm} ppm`);
    if (backend.operation)
        lines.push(`${backend.operation} (HRESULT 0x${backend.hresult?.toString(16) ?? 'unknown'})`);
    return lines.filter(Boolean).join('\n');
}

function drawBackend({backend: {backend, file, error, errorWidth}, left, width, height}) {
    const before = left > 0;
    const y = height / 2;
    const end = before ? left - 2 : width + backendGap - 9;
    const start = before ? left - backendGap + 8 : width + 2;
    const labelX = before ? left - backendGap : width + backendGap;
    const labelY = y + (file || error ? -2 : 3);
    const anchor = before ? 'end' : 'start';
    return `<g class="backend">
        <title>${xml(backendTitle(backend))}</title>
        <path d="M${start} ${y} H${end}" class="backend-link" marker-end="url(#arrow)"/>
        ${text(labelX, labelY, backend.type, `class="backend-label" text-anchor="${anchor}"`)}
        ${file ? text(labelX, labelY + 12, file, `class="backend-file" text-anchor="${anchor}"`) : ''}
        ${error ? tag(labelX - (before ? errorWidth : 0), labelY + (file ? 16 : 4), error, 'error') : ''}
    </g>`;
}

function nodeTitle(node, title, queue) {
    const lines = [`#${node.id} ${title}`, `In execution plan: ${node.in_plan ? 'yes' : 'no'}`,
        `Paused: ${node.paused ? 'yes' : 'no'}`];
    for (const [dir, ports] of [['Inputs', node.inputs], ['Outputs', node.outputs]]) {
        lines.push(`${dir}:${ports.length ? '' : ' none'}`);
        ports.forEach((spec, i) => lines.push(`  ${i}: ${spec.rate} Hz, ${spec.channels} ch, ${spec.format}`));
    }
    node.slots?.forEach((slot, i) => lines.push(`Slot ${i}: high=${!!slot.high}, muted=${!!slot.muted}, suppressed=${!!slot.suppressed}`));
    if (node.gain !== undefined)
        lines.push(`Gain: ${node.gain}`);
    if (node.queue)
        lines.push(`Bound: ${node.queue.bound}, online: ${node.queue.online}, steady: ${node.queue.steady}`,
            ...queue, `Target: ${Math.floor(node.queue.capacity / 2)} frames`);
    return lines.join('\n');
}

function drawPort(node, side, port, x, y, width, count, caption = '') {
    const input = side === 'input';
    const label = `${input ? 'In' : 'Out'} ${port}`;
    const spec = (input ? node.inputs : node.outputs)[port];
    const height = Math.max(portMarkHeight, width);
    const title = `${label}: ${specText(spec)}\n${count} ${count === 1 ? 'connection' : 'connections'}`;
    return `<g id="${side}-${node.id}-${port}">
        <title>${xml(title)}</title>
        <rect class="port ${count ? 'connected' : 'empty'}" x="${x}" y="${value(y - height / 2)}"
              width="4" height="${value(height)}" rx="1"/>
        ${caption}
    </g>`;
}

function drawInputPorts({node, box, incoming}) {
    return node.inputs.map((_, i) => {
        const link = incoming.get(i);
        return drawPort(node, 'input', i, 0, box.ports[i].y, link?.width ?? 0, link ? 1 : 0);
    }).join('');
}

function drawOutputPorts({node, width, height, outputs}, rows) {
    let lastLabel = -Infinity;
    return outputs.map(({port, top, span, branches}) => {
        const label = `Out ${port}`;
        const x = width - 10;
        const labelLeft = x - textWidth(label, 9) - 3;
        // Source cards with multiple outputs already name each port in a slot row.
        const showLabel = (branches.length > 1 || node.outputs.length > 1) && (node.inputs.length || node.outputs.length === 1);
        const y = showLabel
            ? [top + span / 2 + 3, top + span + 6, top - 1].map(y => Math.max(12, Math.min(height - 4, y)))
                .find(y => y - lastLabel >= 11 && rows.every(row => y + 2 <= row.baseline - row.size
                    || y - 9 >= row.baseline + 2 || labelLeft >= 8 + textWidth(row.label, row.size)))
            : undefined;
        if (y !== undefined)
            lastLabel = y;
        const caption = y === undefined ? '' : text(x, y, label, 'class="edge-label" text-anchor="end"');
        return drawPort(node, 'output', port, width - 4, top + span / 2, span - 1, branches.length, caption);
    }).join('');
}

function drawCard(card, position) {
    const {node, width, height, left, title, details, badges, queue, hub, slots, firstSlot, rowHeight} = card;
    const endpoint = node.queue;
    const quiet = inactive(node) || (endpoint && !endpoint.online) || node.gain === 0;
    const fraction = endpoint && !quiet ? queueFraction(endpoint) : 0;
    const warning = endpoint && !quiet && queueWarning(endpoint);
    const fill = quiet ? '#F1F5F9' : endpoint ? '#FFFFFF' : hub ? '#DCE7F0' : '#F1F5F9';
    const border = quiet ? colors.inactive : endpoint ? (warning ? '#B45309' : '#0F766E')
        : hub ? '#536D85' : colors.border;
    const radius = endpoint || hub ? 8 : 6;
    const waterY = height * (1 - fraction);
    const rows = [{label: title, size: 12, baseline: 19, className: 'node-title'},
        ...details.map((label, i) => ({label, size: 10, baseline: 32 + i * 12, className: 'detail'})),
        ...slots.map(({label, tone}, i) => ({label, size: 10, baseline: firstSlot + i * rowHeight + 3.5,
            className: `slot ${tone}`}))];
    const waterLineStart = fraction > 0 ? rows.reduce((start, {label, size, baseline}) =>
        waterY >= baseline - size && waterY <= baseline + 2
            ? Math.max(start, Math.min(width - 10, 12 + textWidth(label, size))) : start, 0) : 0;
    const clip = `water-${node.id}`;
    const water = endpoint ? `<clipPath id="${clip}"><rect width="${width}" height="${height}" rx="${radius}"/></clipPath>
        <g clip-path="url(#${clip})">
            ${fraction > 0 ? `<rect y="${value(waterY)}" width="${width}" height="${value(height - waterY)}"
                fill="${warning ? '#FDE68A' : '#99F6E4'}" fill-opacity="0.58"/>` : ''}
            ${endpoint.capacity ? `<path d="M0 ${height / 2} H${width}" class="target"/>` : ''}
            ${fraction > 0 ? `<path d="M${waterLineStart} ${value(Math.max(0.5, waterY))} H${width}"
                class="water-line" stroke="${border}"/>` : ''}
        </g>` : '';
    let badgeX = 7;
    const badge = badges.map(([label, tone]) => {
        const shape = tag(badgeX, -tagHeight / 2, label, tone);
        badgeX += tagWidth(label) + 3;
        return shape;
    }).join('');
    return `<g id="${nodeKey(node.id)}" transform="translate(${value(position.x)} ${value(position.y)})">
        <title>${xml(nodeTitle(node, title, queue))}</title>
        <g transform="translate(${left} 0)">
            <rect width="${width}" height="${height}" rx="${radius}" fill="${fill}"/>
            ${water}
            <rect class="card" width="${width}" height="${height}" rx="${radius}" stroke="${border}"/>
            ${rows.map(({label, baseline, className}) => text(8, baseline, label, `class="${className}"`)).join('')}
            ${drawInputPorts(card)}
            ${drawOutputPorts(card, rows)}
            ${queue.map((line, i) => text(width / 2, height + 13 + i * 13, line,
                `text-anchor="middle" class="queue ${i ? 'warn-text' : ''}"`)).join('')}
            ${badge}
        </g>
    </g>`;
}

const definitions = `<defs>
    <marker id="arrow" viewBox="0 0 8 8" refX="8" refY="4" markerWidth="5" markerHeight="5" orient="auto">
        <path d="M0 0 L8 4 L0 8 Z" fill="${colors.border}"/>
    </marker>
</defs>
<style>
    text {font-family: Inter, "Segoe UI", Arial, sans-serif; fill: ${colors.ink}}
    .brand {font-size:13px;font-weight:700}
    .node-title {font-size:12px;font-weight:700}
    .detail,.queue,.slot,.backend-file {font-size:10px;font-weight:400}
    .detail {fill:${colors.secondary}}
    .edge-label {font-size:9px;font-weight:500;fill:${colors.secondary}}
    .backend-label,.section-title {font-size:10px;font-weight:700}
    .backend-file {fill:${colors.secondary}}
    .card {fill:none;stroke-width:1.3}
    .target {fill:none;stroke:#94A3B8;stroke-width:1;stroke-dasharray:4 3}
    .water-line {fill:none;stroke-width:1}
    .backend-link {fill:none;stroke:${colors.border};stroke-width:1}
    .port.connected {fill:#8BA4B2}
    .port.empty {fill:#F8FAFC;stroke:#94A3B8;stroke-width:0.8}
    .tag {stroke-width:0.7}
    .tag-label {font-size:9px;font-weight:700}
    .on {fill:#DCFCE7;stroke:#86B69C}
    .ready {fill:#E0EAF4;stroke:#9CB1C5}
    .warn {fill:#FEF3C7;stroke:#D97706}
    .off {fill:#F1F5F9;stroke:#CBD5E1}
    .error {fill:#FEE2E2;stroke:#DC2626}
    .off-text {fill:#64748B}
    .warn-text {fill:#B45309}
    .high-text {fill:#1D4E80;font-weight:700}
</style>`;

function drawRoutedStroke({points, curved}, style) {
    const paint = ribbonPaint[style.palette];
    const graphic = {
        z: style.paused ? -2 : -1, silent: true,
        style: {
            fill: 'none', lineWidth: style.width, lineDash: style.dash, lineJoin: 'round',
            opacity: paint?.[2] ?? 1, stroke: style.color || paint?.[0] || colors.inactive,
        },
    };
    if (!curved)
        return {...graphic, type: 'polyline', shape: {points: points.map(({x, y}) => [x, y]), smooth: 0.06}};
    const [start, end] = points;
    const center = (start.x + end.x) / 2;
    return {...graphic, type: 'bezierCurve', shape: {x1: start.x, y1: start.y, x2: end.x, y2: end.y,
        cpx1: center, cpy1: start.y, cpx2: center, cpy2: end.y}};
}

function renderFlows(layout, boxes, flows, cardsById) {
    if (!flows.length)
        return '';
    const bands = [];
    const graphic = [];
    const labels = [];
    const usedLabels = [];
    const labelledFanouts = new Set();
    const spans = flows.flatMap(({drawing}) => drawing.spans);
    for (const {edge, style, drawing} of flows) {
        if (!drawing.curved || !style.palette) {
            graphic.push(drawRoutedStroke(drawing, style));
            continue;
        }
        const {points} = drawing;
        for (let i = 1; i < points.length; i++)
            bands.push({start: points[i - 1], end: points[i], style});
        if (style.palette === 'muted')
            continue;
        const card = cardsById.get(nodeKey(edge.from[0]));
        const source = card.node;
        const output = source.outputs[edge.from[1]];
        const input = source.inputs.length === 1 ? source.inputs[0] : null;
        const changesChannels = input && input.channels !== output.channels;
        const changesSpec = input && (input.rate !== output.rate || input.format !== output.format);
        const port = card.outputs[edge.from[1]];
        const fanout = port.branches.length > 1;
        if (!changesChannels && !changesSpec && (!fanout || labelledFanouts.has(port)))
            continue;
        const label = changesChannels ? `${input.channels}ch -> ${output.channels}ch`
            : `${output.channels}ch ${output.format}`;
        const point = placeEdgeLabel(textWidth(label, 9) + 8, drawing, style.width, boxes, spans, usedLabels);
        if (point) {
            labels.push(text(point.x, point.y, label, 'class="edge-label" text-anchor="middle"'));
            if (fanout)
                labelledFanouts.add(port);
        }
    }

    // Virtual anchors place adjacent ribbons directly at their group's output port.
    // A zero gap and total band width as layout height keep one layout unit equal to one pixel.
    const height = bands.reduce((sum, {style}) => sum + style.width, 0);
    const data = [];
    const links = [];
    bands.forEach(({start, end, style}, index) => {
        const [sourceColor, targetColor, opacity] = ribbonPaint[style.palette];
        const anchor = (name, point, color) => ({
            name, localX: point.x / layout.width, localY: (point.y - style.width / 2) / height,
            itemStyle: {color},
        });
        const source = `source-${index}`;
        const target = `target-${index}`;
        data.push(anchor(source, start, sourceColor), anchor(target, end, targetColor));
        links.push({source, target, value: style.width,
            lineStyle: {color: sourceColor === targetColor ? sourceColor : 'gradient', opacity}});
    });
    const chart = echarts.init(null, null, {renderer: 'svg', ssr: true,
        width: Math.ceil(layout.width), height: Math.ceil(layout.height)});
    try {
        chart.setOption({animation: false, graphic, series: bands.length ? [{
            type: 'sankey', top: 0, left: 0, width: layout.width, height, nodeGap: 0, nodeWidth: 0,
            layoutIterations: 0, silent: true, emphasis: {disabled: true}, itemStyle: {opacity: 0},
            label: {show: false}, lineStyle: {color: 'gradient', curveness: 0.5, opacity: 1}, data, links,
        }] : []});
        const svg = chart.renderToSVGString();
        return svg.slice(svg.indexOf('>') + 1, svg.lastIndexOf('</svg>')) + labels.join('');
    } finally {
        chart.dispose();
    }
}

function detachedSection(backends, width, y) {
    if (!backends.length)
        return {markup: '', height: 0};
    const rows = [];
    let x = 12;
    let row = 0;
    for (const {backend, error, file, nameWidth, width: itemWidth} of backends) {
        if (x > 12 && x + itemWidth > width - 12) {
            x = 12;
            row++;
        }
        const rowY = 33 + row * 34;
        rows.push(`<g class="detached-backend"><title>${xml(backendTitle(backend))}</title>
            ${text(x, rowY, backend.type, 'class="backend-label"')}
            ${file ? text(x, rowY + 13, file, 'class="backend-file"') : ''}
            ${error ? tag(x + nameWidth + 7, rowY - 10.5, error, 'error') : ''}
        </g>`);
        x += itemWidth + backendGap;
    }
    const height = 58 + row * 34;
    return {height, markup: `<g id="detached-backends" transform="translate(${margin} ${y})">
        <rect width="${width}" height="${height}" rx="6" fill="#FFFFFF" stroke="#CBD5E1" stroke-dasharray="4 4"/>
        ${text(12, 16, 'Detached Backends', 'class="section-title"')}
        ${rows.join('')}
    </g>`};
}

async function render(snapshot) {
    if (snapshot.version !== 1)
        throw new Error(`Unsupported diagnostic JSON version ${snapshot.version}`);
    const {engine, nodes, connections, backends, positions: anchors} = snapshot;
    const nodeById = new Map(nodes.map(node => [node.id, node]));
    const backendByNode = new Map();
    const detached = [];
    for (const backend of backends) {
        const label = backendLabel(backend);
        if (nodeById.has(backend.node)) {
            backendByNode.set(backend.node, label);
        } else {
            label.width = Math.max(label.nameWidth + (label.error ? label.errorWidth + 7 : 0), label.fileWidth);
            detached.push(label);
        }
    }
    const routes = new Map(nodes.map(node => [node.id, {incoming: new Map(), outgoing: []}]));
    const styles = new Map();
    for (const edge of connections) {
        routes.get(edge.from[0]).outgoing.push(edge);
        const source = nodeById.get(edge.from[0]);
        const style = wireStyle(edge, source, nodeById.get(edge.to[0]));
        routes.get(edge.to[0]).incoming.set(edge.to[1], {in_plan: edge.in_plan,
            offline: inactive(source) || (source.queue && !source.queue.online), width: style.width});
        styles.set(edge.id, style);
    }
    const cards = nodes.map(node => {
        const {incoming, outgoing} = routes.get(node.id);
        const ordered = orderFanouts(outgoing, anchors);
        return makeCard(node, backendByNode.get(node.id), incoming, ordered, styles, engine.state === 'Running',
            anchors?.[node.id]);
    });
    const {layout, positioned} = await arrangeGraph(cards, connections, styles, anchors);
    const cardsById = new Map(cards.map(card => [card.box.id, card]));
    const boxes = occupiedAreas(layout, cardsById);
    const flows = planFlows(layout, boxes, connections, styles, positioned);
    const wires = renderFlows(layout, boxes, flows, cardsById);
    const positions = new Map((layout.children || []).map(node => [node.id, node]));
    const external = cards.filter(card => card.backend).map(card => {
        const {x, y} = positions.get(card.box.id);
        return `<g transform="translate(${value(x)} ${value(y)})">${drawBackend(card)}</g>`;
    }).join('');
    const body = cards.length ? wires + cards.map(card => drawCard(card, positions.get(card.box.id))).join('')
        : text(0, 22, 'No graph nodes', 'class="detail"');
    const state = engine.state.toUpperCase();
    const metrics = `${engine.period_ms} ms · Plan #${engine.plan} · tick ${engine.tick} · ${engine.tick_us} us`;
    const tagY = (headerHeight - tagHeight) / 2;
    const stateX = margin + textWidth('AEReborn', 13) + 11;
    const banner = tag(stateX, tagY, state, state === 'RUNNING' ? 'on' : state === 'READY' ? 'ready' : 'off');
    const metricsX = stateX + tagWidth(state) + 12;
    const alerts = [];
    if (engine.dirty)
        alerts.push(['DIRTY', 'warn']);
    if (engine.missed_periods > 0)
        alerts.push([`LATE ${engine.missed_periods}`, 'error']);
    const alertWidth = alerts.reduce((sum, [label]) => sum + tagWidth(label) + 7, 0);
    const width = Math.ceil(Math.max(layout.width + 2 * margin, 480,
        metricsX + textWidth(metrics, 10) + alertWidth + 2 * margin,
        ...detached.map(label => label.width + 2 * margin + 24)));
    let alertX = width - margin - alertWidth + 7;
    const right = alerts.map(([label, tone]) => {
        const markup = tag(alertX, tagY, label, tone);
        alertX += tagWidth(label) + 7;
        return markup;
    }).join('');
    const bodyHeight = Math.max(34, layout.height);
    const footerY = headerHeight + margin + bodyHeight + (detached.length ? 18 : 0);
    const footer = detachedSection(detached, width - 2 * margin, footerY);
    const height = Math.ceil(footerY + footer.height + margin);
    return `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}"
     role="img" aria-labelledby="graph-title">
    <title id="graph-title">AEReborn audio graph</title>
    ${definitions}
    <rect width="${width}" height="${height}" fill="#FFFFFF"/>
    <g id="engine-header">
        <title>${xml(`Engine: ${state}\nPeriod: ${engine.period_ms} ms\nPlan: ${engine.plan}\nTick: ${engine.tick}\nTick time: ${engine.tick_us} us\nDirty: ${engine.dirty}\nMissed periods: ${engine.missed_periods}`)}</title>
        <rect width="${width}" height="${headerHeight}" fill="#F8FAFC"/>
        <path d="M0 ${headerHeight} H${width}" stroke="#E2E8F0" stroke-width="1"/>
        ${text(margin, 20.5, 'AEReborn', 'class="brand"')}
        ${banner}
        ${text(metricsX, 19.5, metrics, 'class="detail"')}
        ${right}
    </g>
    <g id="topology" transform="translate(${margin} ${headerHeight + margin})">${body}<g id="external-backends">${external}</g></g>
    ${footer.markup}
</svg>
`;
}

async function main() {
    const usage = 'Usage: render_graph.js INPUT.json [OUTPUT.svg]';
    const [input, output, ...extra] = process.argv.slice(2);
    if (input === '--help' && !output) {
        console.log(usage);
        return;
    }
    if (!input || input.startsWith('--') || extra.length)
        throw new Error(usage);
    const outputPath = output ?? path.join(path.dirname(input), `${path.parse(input).name}.svg`);
    if (path.resolve(input) === path.resolve(outputPath))
        throw new Error('Input and output files must be different.');
    const snapshot = JSON.parse(fs.readFileSync(input, 'utf8'));
    fs.writeFileSync(outputPath, await render(snapshot), 'utf8');
}

if (require.main === module) {
    main().catch(error => {
        console.error(error.message);
        process.exitCode = 1;
    });
}

module.exports = {render};
