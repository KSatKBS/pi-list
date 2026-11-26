// Browser check of the LIST GUI: logs in, opens every stream of the given
// pcaps and clicks each tab (Analysis / Stream Explorer / Graphs), reporting
// JavaScript errors and blank pages.
//
// Usage: node gui_check.js <base url> <username> <password> <pcap id>...
// Needs playwright-core and a Chrome binary (CHROME_PATH, default /usr/bin/google-chrome).

const { chromium } = require('playwright-core');

const [base, username, password, ...pcapIds] = process.argv.slice(2);
const TABS = ['Analysis', 'Stream Explorer', 'Graphs'];

async function api(token, path) {
    const r = await fetch(`${base}${path}`, { headers: { Authorization: `Bearer ${token}` } });
    if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
    return r.json();
}

(async () => {
    const login = await fetch(`${base}/auth/login`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password }),
    }).then((r) => r.json());
    const token = login.content.token;

    const browser = await chromium.launch({ executablePath: process.env.CHROME_PATH || '/usr/bin/google-chrome' });
    const page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
    await page.addInitScript(() => localStorage.setItem('gdprConsent', 'false'));

    const problems = [];
    let where = 'login';
    page.on('pageerror', (e) => problems.push(`[${where}] page error: ${e.message}\n${(e.stack || '').split('\n').slice(0, 6).join('\n')}`));
    page.on('console', (m) => {
        if (m.type() === 'error') console.log(`  [${where}] console error: ${m.text().slice(0, 400)}`);
    });

    const checkNotBlank = async () => {
        await page.waitForTimeout(3000);
        const text = (await page.evaluate(() => document.body.innerText || '')).trim();
        if (text.length < 20) problems.push(`[${where}] blank page (text: "${text}")`);
    };

    // UI login
    await page.goto(`${base}/login`, { waitUntil: 'networkidle' });
    await page.fill('input.login-page-username-input', username);
    await page.fill('input.login-page-password-input', password);
    await page.click('a.login-page-buttons:has-text("Login")');
    await page.waitForURL('**/pcaps', { timeout: 30000 });
    where = 'dashboard';
    await checkNotBlank();
    console.log('dashboard OK');

    for (const pcapId of pcapIds) {
        const pcap = await api(token, `/api/pcap/${pcapId}`);
        const streams = await api(token, `/api/pcap/${pcapId}/streams`);
        for (const s of streams) {
            const label = `${pcap.file_name} / ${s.media_type}`;
            where = label;
            await page.goto(`${base}/pcaps/${pcapId}/streams/${s.id}`, { waitUntil: 'networkidle' });
            await checkNotBlank();
            for (const tab of TABS) {
                const el = page.getByText(tab, { exact: true }).first();
                if (!(await el.count()) || !(await el.isVisible())) continue;
                where = `${label} / ${tab}`;
                await el.click();
                await checkNotBlank();
                console.log(`checked ${where}`);
            }
        }
    }

    await browser.close();

    if (problems.length) {
        console.log(`\n${problems.length} GUI problem(s):\n${problems.join('\n')}`);
        process.exit(1);
    }
    console.log('GUI check OK');
})().catch((e) => {
    console.error(e);
    process.exit(1);
});
