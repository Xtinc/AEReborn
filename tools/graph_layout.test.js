'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {arrangeGraph, cardBox, inactive, nodeKey, occupiedAreas, planFlows, ribbonWidth, slotTop} = require('./graph_layout');
const {render} = require('./render_graph');
const reference = require('../examples/sankey_reference.json');
const {nodes, connections} = reference;

test('inactive nodes move below the active graph without displacing its routes', async () => {
    const byId = new Map(nodes.map(node => [node.id, node]));
    const styles = new Map(connections.map(edge => [edge.id, {
        width: edge.in_plan ? ribbonWidth(byId.get(edge.from[0]).outputs[edge.from[1]].channels) : 1.5,
    }]));
    const cards = nodes.map(node => {
        const outgoing = connections.filter(edge => edge.from[0] === node.id);
        const dimensions = cardBox(node, 128, 74, 0, 0, !!node.slots, slotTop, outgoing, styles);
        return {node, width: 128, ...dimensions};
    });
    const {layout} = await arrangeGraph(cards, connections, styles);
    const boxes = new Map(layout.children.map(box => [box.id, box]));
    const activeBottom = Math.max(...nodes.filter(node => !inactive(node)).map(node => {
        const box = boxes.get(nodeKey(node.id));
        return box.y + box.height;
    }));
    for (const node of nodes.filter(inactive))
        assert.ok(boxes.get(nodeKey(node.id)).y > activeBottom);

    const anchors = new Map(layout.children.flatMap(box => box.ports.map(port =>
        [port.id, {x: box.x + port.x, y: box.y + port.y}])));
    for (const edge of connections) {
        const sections = layout.edges.find(route => route.id === String(edge.id)).sections;
        const start = sections[0].startPoint;
        const end = sections[sections.length - 1].endPoint;
        const expectedStart = anchors.get(`out-${edge.id}`);
        const expectedEnd = anchors.get(`in-${edge.to[0]}-${edge.to[1]}`);
        assert.ok(Math.hypot(start.x - expectedStart.x, start.y - expectedStart.y) < 0.001, `Edge #${edge.id} source`);
        assert.ok(Math.hypot(end.x - expectedEnd.x, end.y - expectedEnd.y) < 0.001, `Edge #${edge.id} target`);
    }
});

test('the SVG includes every physical port, including empty mixer inputs and unused outputs', async () => {
    const svg = await render({...reference, connections: connections.filter(edge => edge.to[0] !== 10 || edge.to[1] !== 3)});
    const expected = nodes.flatMap(node => [
        ...node.inputs.map((_, i) => `input-${node.id}-${i}`),
        ...node.outputs.map((_, i) => `output-${node.id}-${i}`),
    ]).sort();
    const markers = [...svg.matchAll(/id="((?:input|output)-\d+-\d+)"/g)].map(match => match[1]).sort();
    assert.deepEqual(markers, expected);
    assert.match(svg, /id="input-10-3">\s*<title>In 3:[^\n]*\n0 connections<\/title>\s*<rect class="port empty"/);
    assert.match(svg, /id="output-11-0">\s*<title>Out 0:[^\n]*\n0 connections<\/title>\s*<rect class="port empty"/);
    assert.match(svg, /id="output-1-0">\s*<title>Out 0:[^\n]*\n2 connections<\/title>/);
});

test('parallel ribbons share an output span and preserve distinct mixer input slots', async () => {
    const stereo = {channels: 2};
    const nodes = [
        {id: 1, in_plan: true, inputs: [], outputs: [stereo, stereo]},
        {id: 2, in_plan: true, inputs: [stereo, stereo], outputs: [stereo]},
        {id: 3, in_plan: false, inputs: [stereo], outputs: []},
    ];
    const edges = [
        {id: 1, from: [1, 0], to: [2, 0]},
        {id: 2, from: [1, 0], to: [3, 0]},
        {id: 3, from: [1, 1], to: [2, 1]},
    ];
    const active = {width: ribbonWidth(2), palette: 'flow'};
    const styles = new Map([[1, active], [2, {width: 1.5, paused: true}], [3, active]]);
    const cards = nodes.map(node => ({
        node, width: 96, ...cardBox(node, 96, 74, 0, 0, node.id !== 3, slotTop,
            edges.filter(edge => edge.from[0] === node.id), styles),
    }));
    const {layout} = await arrangeGraph(cards, edges, styles, {1: [0, 8], 2: [210, 8], 3: [210, 155]});
    const cardsById = new Map(cards.map(card => [card.box.id, card]));
    const flows = planFlows(layout, occupiedAreas(layout, cardsById), edges, styles, true);
    const [first, second] = cards[0].outputs;
    assert.deepEqual(first.branches.map(branch => branch.id), [1, 2]);
    assert.deepEqual(second.branches.map(branch => branch.id), [3]);
    assert.ok(first.top + first.span < second.top);
    const starts = flows.map(flow => flow.drawing.points[0]);
    assert.deepEqual(starts.map(point => point.x), [96, 96, 96]);
    assert.equal(starts[0].y - active.width / 2, 8 + first.top + 0.5);
    assert.equal(starts[1].y + styles.get(2).width / 2, 8 + first.top + first.span - 0.5);
    assert.equal(starts[2].y, 8 + slotTop + cards[0].rowHeight);
    assert.deepEqual(flows[0].drawing.points.at(-1), {x: 210, y: 8 + slotTop});
    assert.deepEqual(flows[2].drawing.points.at(-1), {x: 210, y: 8 + slotTop + cards[1].rowHeight});
});
