'use strict';

const ELK = require('elkjs/lib/elk.bundled.js');

const slotTop = 47;
const slotHeight = 17;
const rowGap = 20;
const columnGap = 80;
const outputPortGap = 6;
const portMarkHeight = 7;
const nodeKey = id => `node-${id}`;
const inputKey = (node, port) => `in-${node}-${port}`;
const outputKey = id => `out-${id}`;
function ribbonWidth(channels) {
    if (channels <= 1)
        return 7;
    if (channels <= 4)
        return 16 + (channels - 2) * 2;
    return Math.min(36, 20 + (channels - 4));
}
const inactive = node => !node.in_plan;

function cardBox(node, width, minHeight, sideWidth, queueHeight, hub, firstSlot, outgoing, styles) {
    const slotPorts = hub ? (node.inputs.length ? node.inputs : node.outputs) : [];
    const outputs = node.outputs.map((_, port) => ({port, span: 0, branches: []}));
    for (const edge of outgoing) {
        const output = outputs[edge.from[1]];
        const span = styles.get(edge.id).width + 1;
        output.span += span;
        output.branches.push({id: edge.id, to: edge.to, span});
    }
    for (const output of outputs)
        if (!output.branches.length)
            output.span = portMarkHeight + 1;
    const outputSpan = outputs.reduce((sum, output) => sum + output.span, 0)
        + Math.max(0, outputs.length - 1) * outputPortGap;
    const portHeight = slotPorts.reduce((height, spec) => Math.max(height, ribbonWidth(spec.channels) + 3), slotHeight);
    const outputRows = hub && !node.inputs.length;
    const rowHeight = outputRows ? outputs.reduce((height, output) => Math.max(height, output.span + outputPortGap),
        portHeight) : portHeight;
    if (outputRows)
        firstSlot = Math.max(firstSlot, rowHeight / 2 + 3);
    const left = node.inputs.length ? 0 : sideWidth;
    const slotBottom = outputRows ? rowHeight / 2 + 3 : 10;
    const height = Math.ceil(Math.max(minHeight,
        slotPorts.length ? firstSlot + (slotPorts.length - 1) * rowHeight + slotBottom : 0, outputSpan + 6));
    const box = {
        id: nodeKey(node.id), width: width + sideWidth, height: height + queueHeight,
        layoutOptions: {
            'elk.portConstraints': 'FIXED_POS',
            'elk.layered.layering.layerConstraint': node.inputs.length ? 'NONE' : 'FIRST',
        },
        ports: node.inputs.map((_, i) => ({
            id: inputKey(node.id, i), x: 0, y: hub ? firstSlot + i * rowHeight
                : height * (i + 1) / (node.inputs.length + 1),
            width: 0, height: 0, layoutOptions: {'elk.port.side': 'WEST'},
        })),
    };
    let y = (height - outputSpan) / 2;
    for (const output of outputs) {
        if (outputRows)
            y = firstSlot + output.port * rowHeight - output.span / 2;
        output.top = y;
        for (const branch of output.branches) {
            box.ports.push({id: outputKey(branch.id), x: width + sideWidth, y: y + branch.span / 2,
                width: 0, height: 0, layoutOptions: {'elk.port.side': 'EAST'}});
            y += branch.span;
        }
        y = output.top + output.span + outputPortGap;
    }
    return {height, left, box, firstSlot, rowHeight, outputs};
}

// The ECharts curve with 0.5 curvature is monotone on both axes.
function curvePoint(start, end, t) {
    return {
        x: start.x + (end.x - start.x) * t * (1.5 - t * (1.5 - t)),
        y: start.y + (end.y - start.y) * t * t * (3 - 2 * t),
    };
}

function spanBox(start, end, pad) {
    return {x: Math.min(start.x, end.x), y: Math.min(start.y, end.y) - pad,
        width: Math.abs(end.x - start.x), height: Math.abs(end.y - start.y) + 2 * pad};
}

function curveSpans(start, end, pad) {
    const spans = [];
    let previous = start;
    for (let i = 1; i <= 24; i++) {
        const point = curvePoint(start, end, i / 24);
        spans.push(spanBox(previous, point, pad));
        previous = point;
    }
    return spans;
}

function overlaps(a, b, topPadding = 0) {
    return a.x < b.x + b.width && a.x + a.width > b.x
        && a.y < b.y + b.height && a.y + a.height > b.y - topPadding;
}

function curvedRoute(points, width, obstacles) {
    const spans = [];
    for (let i = 1; i < points.length; i++) {
        const start = points[i - 1];
        const end = points[i];
        if (end.x <= start.x)
            return null;
        const segment = curveSpans(start, end, width / 2 + 3);
        if (segment.some(span => obstacles.some(box => overlaps(span, box, 7))))
            return null;
        spans.push(...segment);
    }
    return {points, spans, curved: true};
}

function followBridges(edge, anchors, routes) {
    const chain = [];
    while (!anchors[edge.to[0]]) {
        const id = edge.to[0];
        const outgoing = routes.get(id).outgoing;
        if (outgoing.length !== 1 || chain.includes(id))
            break;
        chain.push(id);
        edge = outgoing[0];
    }
    return {chain, edge};
}

function placeBridges(cards, connections, anchors, routes) {
    const positions = {...anchors};
    const cardById = new Map(cards.map(card => [card.node.id, card]));
    for (const connection of connections) {
        const source = anchors[connection.from[0]];
        if (!source)
            continue;
        const {chain, edge} = followBridges(connection, anchors, routes);
        const target = anchors[edge.to[0]];
        if (!chain.length || !target)
            continue;
        const fromRight = source[0] + cardById.get(connection.from[0]).width;
        const bridges = chain.map(id => cardById.get(id));
        const available = target[0] - fromRight - bridges.reduce((sum, card) => sum + card.width, 0);
        const gap = Math.min(columnGap, available / (bridges.length + 1));
        if (gap < 12)
            throw new Error(`Layout has no room for the bridges on connection #${connection.id}`);
        const targetPort = cardById.get(edge.to[0]).box.ports.find(port => port.id === inputKey(...edge.to));
        const lane = target[1] + targetPort.y;
        let x = target[0] - gap;
        // End each automatic conversion chain on the destination's signal lane.
        for (const card of bridges.reverse()) {
            x -= card.width;
            positions[card.node.id] = [x, lane - card.height / 2];
            x -= gap;
        }
    }
    const top = Math.min(...Object.values(positions).map(([, y]) => y));
    return Object.fromEntries(Object.entries(positions).map(([id, [x, y, ...size]]) => [id, [x, y - top + 8, ...size]]));
}

function placedLayout(cards, connections, positions, routed = new Map()) {
    const children = cards.map(({node, box, left}) => {
        const position = positions[node.id];
        if (!Array.isArray(position) || ![2, 4].includes(position.length) || !position.every(Number.isFinite)
            || position.slice(2).some(size => size <= 0))
            throw new Error(`Expected [x, y] or [x, y, width, height] for node #${node.id}`);
        return {...box, x: position[0] - left, y: position[1]};
    });
    const left = Math.min(0, ...children.map(box => box.x));
    const top = Math.min(0, ...children.map(box => box.y - 8));
    const ports = new Map();
    for (const box of children) {
        box.x -= left;
        box.y -= top;
        for (const port of box.ports)
            ports.set(port.id, {x: box.x + port.x, y: box.y + port.y});
    }
    const edges = connections.map(edge => ({
        id: String(edge.id),
        sections: routed.get(edge.id) || [{startPoint: ports.get(outputKey(edge.id)), endPoint: ports.get(inputKey(...edge.to))}],
    }));
    return {
        children, edges,
        width: Math.max(0, ...children.map(box => box.x + box.width)),
        height: Math.max(0, ...children.map(box => box.y + box.height)),
    };
}

function spreadBends(path) {
    const points = path.map(point => ({...point}));
    for (let i = 1; i + 1 < points.length; i++) {
        if (points[i].x !== points[i + 1].x || i + 2 === points.length)
            continue;
        const x = points[i].x;
        const gap = Math.min(12, (x - points[i - 1].x) / 2, (points[i + 2].x - x) / 2);
        points[i].x -= gap;
        points[i + 1].x += gap;
    }
    return points;
}

function avoidCards(start, end, width, obstacles) {
    const pad = width / 2 + 4;
    const blocked = (a, b) => obstacles.some(box => Math.max(a.x, b.x) >= box.x - pad
        && Math.min(a.x, b.x) <= box.x + box.width + pad && Math.max(a.y, b.y) >= box.y - 7 - pad
        && Math.min(a.y, b.y) <= box.y + box.height + pad);
    const clear = points => points.slice(1).every((point, i) => !blocked(points[i], point));
    const mid = (start.x + end.x) / 2;
    const rails = [mid, start.x + 10, end.x - 10, ...obstacles.flatMap(box => [box.x - pad - 4, box.x + box.width + pad + 4])]
        .filter(x => x > start.x && x < end.x).sort((a, b) => Math.abs(a - mid) - Math.abs(b - mid));
    for (const x of rails) {
        const path = [start, {x, y: start.y}, {x, y: end.y}, end];
        if (clear(path))
            return path;
    }
    const fromRails = [start.x + 8, ...rails];
    const toRails = [end.x - 8, ...rails];
    const lanes = obstacles.flatMap(box => [box.y - 7 - pad - 2, box.y + box.height + pad + 2])
        .filter(y => y >= 0).sort((a, b) => Math.abs(a - end.y) - Math.abs(b - end.y));
    for (const y of lanes.slice(0, 8))
        for (const x1 of fromRails.slice(0, 8))
            for (const x2 of toRails.slice(0, 8)) {
                const path = [start, {x: x1, y: start.y}, {x: x1, y}, {x: x2, y}, {x: x2, y: end.y}, end];
                if (clear(path))
                    return path;
            }
    return [start, end];
}

function routeSection({startPoint: start, endPoint: end, bendPoints}, width, obstacles, ribbon) {
    const direct = curvedRoute([start, end], width, obstacles);
    if (direct)
        return direct;
    const points = bendPoints?.length ? [start, ...bendPoints, end] : avoidCards(start, end, width, obstacles);
    if (ribbon && bendPoints?.length) {
        const smooth = curvedRoute(spreadBends(points), width, obstacles);
        if (smooth)
            return smooth;
    }
    const pad = width / 2 + 3;
    const spans = points.slice(1).map((end, i) => {
        const box = spanBox(points[i], end, pad);
        return {...box, x: box.x - pad, width: box.width + 2 * pad};
    });
    return {points, spans, curved: false};
}

function placeEdgeLabel(labelWidth, {points}, width, boxes, wires, used) {
    let segment = 0;
    for (let i = 1; i + 1 < points.length; i++) {
        if (points[i + 1].x - points[i].x > points[segment + 1].x - points[segment].x)
            segment = i;
    }
    const [start, end] = [points[segment], points[segment + 1]];
    if (end.x - start.x < labelWidth + 30)
        return null;
    for (const t of [0.5, 0.38, 0.62]) {
        const point = curvePoint(start, end, t);
        const {x} = point;
        const y = point.y - width / 2 - 6;
        const labelBox = {x: x - labelWidth / 2, y: y - 10, width: labelWidth, height: 12};
        if (boxes.some(box => overlaps(labelBox, box, 8)) || wires.some(span => overlaps(labelBox, span))
            || used.some(box => overlaps(labelBox, box)))
            continue;
        used.push(labelBox);
        return {x, y};
    }
    return null;
}

function occupiedAreas(layout, cardsById) {
    return (layout.children || []).map(box => {
        const card = cardsById.get(box.id);
        return card.backend ? box : {...box, x: box.x + card.left, width: card.width};
    });
}

function planFlows(layout, boxes, connections, styles, positioned) {
    const edgesById = new Map(connections.map(edge => [edge.id, edge]));
    return (layout.edges || []).flatMap(route => {
        const edge = edgesById.get(Number(route.id));
        const style = styles.get(edge.id);
        const from = nodeKey(edge.from[0]);
        const to = nodeKey(edge.to[0]);
        const obstacles = boxes.filter(box => box.id !== from && box.id !== to);
        return route.sections.map(section => {
            const drawing = routeSection(section, style.width, obstacles, !!style.palette);
            if (positioned && style.palette && !drawing.curved)
                throw new Error(`Connection #${edge.id} is blocked by the fixed node placement`);
            return {edge, style, drawing};
        });
    });
}

function stackFanouts(cards, layout) {
    const targets = new Map();
    for (const box of layout.children || [])
        for (const port of box.ports || [])
            targets.set(port.id, box.y + port.y);

    let changed = false;
    for (const card of cards) {
        const ports = new Map(card.box.ports.map(port => [port.id, port]));
        for (const {branches, top} of card.outputs) {
            if (branches.length < 2)
                continue;
            const output = branch => ports.get(outputKey(branch.id));
            const y = branch => targets.get(inputKey(...branch.to)) ?? Number.MAX_SAFE_INTEGER;
            branches.sort((a, b) => y(a) - y(b) || output(a).y - output(b).y);
            let cursor = top;
            for (const branch of branches) {
                const port = output(branch);
                const center = cursor + branch.span / 2;
                changed ||= Math.abs(port.y - center) > 0.001;
                port.y = center;
                cursor += branch.span;
            }
        }
    }
    return changed;
}

async function automaticLayout(cards, connections, styles) {
    const empty = {children: [], edges: [], width: 0, height: 0};
    if (!cards.length)
        return empty;
    const elk = new ELK();
    const run = (cards, edges) => elk.layout({
        id: 'audio',
        layoutOptions: {
            'elk.algorithm': 'layered', 'elk.direction': 'RIGHT', 'elk.edgeRouting': 'ORTHOGONAL',
            'elk.spacing.nodeNode': String(rowGap), 'elk.spacing.edgeNode': '16',
            'elk.layered.spacing.nodeNodeBetweenLayers': String(columnGap),
            'elk.layered.spacing.edgeNodeBetweenLayers': '22',
            'elk.layered.spacing.edgeEdgeBetweenLayers': '12',
            'elk.layered.crossingMinimization.strategy': 'LAYER_SWEEP',
            'elk.layered.considerModelOrder.components': 'MODEL_ORDER',
            'elk.layered.compaction.postCompaction.strategy': 'LEFT',
        },
        children: cards.map(card => structuredClone(card.box)),
        edges: edges.map(edge => ({
            id: String(edge.id), sources: [outputKey(edge.id)], targets: [inputKey(...edge.to)],
            layoutOptions: {'elk.edge.thickness': String(styles.get(edge.id).width)},
        })),
    });
    const active = cards.filter(card => !inactive(card.node));
    let primary = empty;
    if (active.length) {
        const activeIds = new Set(active.map(card => card.node.id));
        const activeEdges = connections.filter(edge => activeIds.has(edge.from[0]) && activeIds.has(edge.to[0]));
        primary = await run(active, activeEdges);
        // Repack only if the target anchors from ELK change the fan-out order.
        if (cards.some(card => card.outputs.some(output => output.branches.length > 1)) && stackFanouts(cards, primary))
            primary = await run(active, activeEdges);
    }
    if (active.length === cards.length)
        return primary;
    const draft = await run(cards, connections);
    const draftPositions = new Map(draft.children.map(box => [box.id, box]));
    const owners = new Map(cards.map(card => [card.box.id, card]));
    const positions = Object.fromEntries((primary.children || []).map(box => {
        const card = owners.get(box.id);
        return [card.node.id, [box.x + card.left, box.y]];
    }));
    const bottom = active.length ? primary.height + rowGap : 12;
    let cursor = 12;
    const idle = cards.filter(card => inactive(card.node)).sort((a, b) => draftPositions.get(a.box.id).x
        - draftPositions.get(b.box.id).x || a.node.id - b.node.id);
    for (const card of idle) {
        const x = Math.max(cursor, draftPositions.get(card.box.id).x);
        positions[card.node.id] = [x + card.left, bottom];
        cursor = x + card.box.width + rowGap;
    }
    const routed = new Map((primary.edges || []).map(edge => [Number(edge.id), edge.sections]));
    return placedLayout(cards, connections, positions, routed);
}

function orderFanouts(outgoing, anchors, routes) {
    if (outgoing.length < 2)
        return outgoing;
    const targetRow = edge => {
        if (!anchors)
            return edge.to[0];
        const target = followBridges(edge, anchors, routes).edge.to[0];
        return anchors[target]?.[1] ?? 0;
    };
    return [...outgoing].sort((a, b) => a.from[1] - b.from[1] || targetRow(a) - targetRow(b)
        || a.to[0] - b.to[0] || a.to[1] - b.to[1]);
}

function guidePositions(nodes, guide) {
    const positions = {};
    for (const node of nodes) {
        const entry = guide[node.name];
        if (entry)
            positions[node.id] = Array.isArray(entry) ? entry : entry[inactive(node) ? 'idle' : 'active'];
    }
    return positions;
}

async function arrangeGraph(cards, connections, styles, anchors, routes, guide) {
    const sourceGutter = cards.reduce((width, card) => Math.max(width, card.left), 0);
    for (const card of cards.filter(card => !card.node.inputs.length)) {
        const padding = sourceGutter - card.left;
        card.left = sourceGutter;
        card.box.width += padding;
        for (const port of card.box.ports)
            port.x += padding;
    }
    const fixed = guide ? placeBridges(cards, connections, anchors, routes) : anchors;
    const layout = fixed ? placedLayout(cards, connections, fixed) : await automaticLayout(cards, connections, styles);
    return {layout, positioned: !!fixed};
}

module.exports = {
    slotTop, portMarkHeight, nodeKey, ribbonWidth, inactive, cardBox, guidePositions, orderFanouts, arrangeGraph, occupiedAreas,
    planFlows, placeEdgeLabel,
};
