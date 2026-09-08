// Run against scripts/preview.py. Uses a separate headless Chrome profile.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const base = process.env.WPS_PREVIEW_URL || 'http://127.0.0.1:8765';
const output = path.resolve(__dirname, '../.pio/ui-review');
fs.mkdirSync(output, {recursive: true});
(async () => {
  const browser = await chromium.launch({channel: 'chrome', headless: true});
  const context = await browser.newContext({viewport: {width: 1440, height: 1050}, colorScheme: 'light'});
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(base);
    await page.waitForFunction(() => document.getElementById('pressure-value').textContent === '2,47');
    assert.equal(await page.locator('#gauge-dot').isVisible(), true);
    assert.equal(await page.locator('#gauge-arc').getAttribute('stroke-dasharray'), '73.18518518518519 100');
    for (const theme of ['light', 'dark']) {
      if (await page.locator('html').getAttribute('data-theme') !== theme) await page.locator('#theme-toggle').click();
      for (const width of [360, 390, 768, 1280, 1440]) {
        await page.setViewportSize({width, height: width < 500 ? 844 : 1050});
        for (const section of ['overview', 'connection', 'calibration', 'device']) {
          await page.locator(`.navigation a[href="#${section}"]`).click();
          await page.locator(`#${section}`).waitFor({state:'visible'});
          assert.equal(await page.locator(`#${section}`).isVisible(), true);
          const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
          assert.equal(overflow, false, `${theme} ${width}px ${section}: horizontal overflow`);
          const inputSize = await page.locator(`#${section} input:not([type=checkbox]):not([type=file])`).evaluateAll(nodes => nodes.every(node => parseFloat(getComputedStyle(node).fontSize) >= 16));
          assert.ok(inputSize, 'Mobile form font size');
          if (section === 'overview' && [390,1440].includes(width)) {
            await page.screenshot({path: path.join(output, `${theme}-${width}.png`), fullPage: width >= 500});
          }
        }
      }
    }
    await page.reload();
    assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark', 'Theme persists after reload');
    await page.locator('#auth-button').click();
    await page.locator('#login-password').fill('wrong');
    await page.locator('#login-form [type=submit]').click();
    await page.getByText('Неверный пароль интерфейса.', {exact:true}).waitFor();
    await page.locator('#login-password').fill('preview-access123');
    await page.locator('#login-form [type=submit]').click();
    await page.waitForFunction(() => !document.getElementById('mqttHost').disabled && document.getElementById('mqttHost').value.length > 0);
    await page.locator('.navigation a[href="#connection"]').click();
    await page.locator('#mqttHost').fill('draft.example.local');
    await page.waitForTimeout(4500);
    assert.equal(await page.locator('#mqttHost').inputValue(), 'draft.example.local', 'Polling preserves unsaved edits');
    await page.locator('#mqttTlsMode').selectOption('2');
    assert.equal(await page.locator('#mqttFingerprint').isVisible(), true);
    await page.locator('#mqttTlsMode').selectOption('0');
    assert.equal(await page.locator('#mqttPort').inputValue(), '1883');
    const saved = page.waitForRequest(request => request.url().endsWith('/api/settings') && request.method() === 'POST');
    await page.locator('#mqtt-form [type=submit]').click();
    const payload = (await saved).postDataJSON();
    assert.equal(payload.mqttHost, 'draft.example.local');
    assert.equal('mqttPass' in payload, false, 'Blank password preserves stored secret');
    await page.locator('#scan-button').click();
    await page.locator('#network-list button').first().waitFor();
    await page.locator('#network-list button').last().click();
    assert.equal(await page.locator('#wifi-ssid').inputValue(), 'Guest');
    assert.equal(await page.locator('#wifi-keep').isChecked(), false);
    await page.locator('.navigation a[href="#calibration"]').click();
    await page.locator('#adcFullScaleV').fill('5.4');
    assert.equal(await page.locator('#calibration-range').textContent(), '0–5,00');
    await page.setViewportSize({width:390,height:844});
    await page.screenshot({path:path.join(output,'dark-calibration-390.png'),fullPage:true});
    await page.route('**/api/status', async route => {
      const response = await route.fetch();
      const data = await response.json();
      data.reading.over = true; data.reading.bar = null; data.reading.mpa = null;
      await route.fulfill({json:data});
    });
    await page.locator('.navigation a[href="#overview"]').click();
    await page.waitForFunction(() => document.getElementById('pressure-value').textContent === '—');
    assert.ok((await page.locator('#measurement-state').textContent()).includes('Выход за диапазон'));
    await page.unroute('**/api/status');
    await page.route('**/api/status', route => route.abort());
    await page.waitForFunction(() => document.getElementById('live-badge').textContent === 'Нет связи', {timeout:12000});
    assert.equal(await page.locator('#pressure-value').textContent(), '—');
    assert.equal(await page.locator('#diag-volts').textContent(), '—');
    await page.unroute('**/api/status');
    await page.waitForFunction(() => document.getElementById('pressure-value').textContent === '2,47');
    await page.locator('#auth-button').click();
    await page.waitForFunction(() => document.querySelector('[data-protected]').disabled);
    assert.deepEqual(errors, [], 'No browser JavaScript exceptions');
    console.log(`PASS: Chrome ${browser.version()}, 40 layouts; themes, auth, forms, Wi-Fi scan, calibration, fault and reconnect. Screenshots: ${output}`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode=1; });
