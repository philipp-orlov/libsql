#!/usr/bin/env python3
"""Writes the demo's report definitions into sales_demo.sqldb.reports/ (one JSON file each), where the sql tool looks for
them: `sql demo/sales_demo.sqldb --listen 8080` then offers them on its Reports tab and at /reports.

    python3 make_reports.py

Besides charts and tables the reports use the newer items: rich text and KPI tiles, columns side by side (with cards), pictures
(a logo and a banner kept in the report, category icons read from the database), cross-tabs with color scales, drill-through
(click a bar, a table row or a cross-tab cell) and sub-reports (Regional Review and Product Catalog repeat another report).
"""
import json
import os

import assets

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sales_demo.sqldb.reports')
os.makedirs(OUT, exist_ok=True)

NAVY, BLUE, TEAL, ORANGE, GREEN, PURPLE, RED, GREY = '#0d47a1', '#1976d2', '#00897b', '#fb8c00', '#43a047', '#8e24aa', '#e53935', '#546e7a'
MUTED = '#6b7280'
LOGO = assets.LOGO
BANNER = assets.data_uri('image/png', assets.banner())
money = '$#,##0'


# ---- building blocks -----------------------------------------------------------------------------------------------------
def rich(text, dataset=None, **style):
    item = {'type': 'text', 'rich': True, 'text': text}
    if dataset:
        item['dataset'] = dataset
    if style:
        item['style'] = style
    return item


def title_bar(title, subtitle, color=NAVY, dataset=None):
    """The logo and a title with a line of small print under it, then a rule."""
    return [
        {'type': 'columns', 'gap': 8, 'columns': [
            {'width': 1, 'items': [{'type': 'image', 'src': LOGO, 'width': 34}]},
            {'width': 14, 'items': [rich('[size=20][b][color=' + color + ']' + title + '[/color][/b][/size]\n[size=9][color=' + MUTED + ']' + subtitle + '[/color][/size]', dataset)]},
        ]},
        {'type': 'rule'},
    ]


def heading(text, note=''):
    return rich('[size=12][b][color=#263238]' + text + '[/color][/b][/size]' + (('   [size=9][color=' + MUTED + ']' + note + '[/color][/size]') if note else ''))


def delta(cur, prev, label):
    """'▲ 12.5% vs ...' when there is something to compare with."""
    return ('{{Iif(First(%s) > 0, Iif(First(%s) >= First(%s), "▲ ", "▼ ") + Format(Abs(First(%s) / First(%s) - 1), "0.0%%") + " %s", "")}}'
            % (prev, cur, prev, cur, prev, label))


def tile(label, value, sub, dataset, accent, size=22):
    """A KPI card: a small caption, a big number, a line of small print."""
    return {'width': 1, 'card': True, 'accent': accent, 'items': [
        rich('[size=8][color=' + MUTED + ']' + label.upper() + '[/color][/size]\n[size=' + str(size) + '][b]' + value + '[/b][/size]\n[size=8][color=' + MUTED + ']' + sub + '[/color][/size]', dataset)]}


def tiles(*cells):
    return {'type': 'columns', 'gap': 10, 'columns': list(cells)}


def side_by_side(*cells, gap=12):
    """cells: (weight, [items]) or (weight, [items], card)."""
    return {'type': 'columns', 'gap': gap, 'columns': [({'width': c[0], 'items': c[1], 'card': True} if len(c) > 2 and c[2] else {'width': c[0], 'items': c[1]}) for c in cells]}


def drill(report, **params):
    return {'report': report, 'params': params}


def report(name, title, description, parameters, datasets, body, landscape=False):
    return {
        'format': 1, 'name': name, 'title': title, 'description': description,
        'page': {'size': 'A4', 'orientation': 'landscape' if landscape else 'portrait', 'margins': {'top': 40, 'right': 36, 'bottom': 40, 'left': 36}},
        'parameters': parameters, 'datasets': datasets,
        'header': [{'type': 'text', 'text': '{{ReportName}}', 'style': {'face': 'bold', 'size': 8, 'color': MUTED}}, {'type': 'rule'}],
        'footer': [{'type': 'rule'}, {'type': 'text', 'text': 'Aurelia Systems · fictional demo data   ·   Page {{PageNumber}} of {{PageCount}}', 'style': {'size': 8, 'color': MUTED, 'align': 'right'}}],
        'body': body,
    }


YEAR = {'name': 'year', 'label': 'Year', 'type': 'integer', 'default': 2026, 'required': True, 'optionsSql': 'SELECT DISTINCT year FROM sales ORDER BY 1 DESC'}
REGION = {'name': 'region', 'label': 'Region', 'type': 'text', 'default': 'Europe', 'required': True, 'optionsSql': 'SELECT DISTINCT region FROM customers ORDER BY 1'}
CATEGORY = {'name': 'category', 'label': 'Category', 'type': 'text', 'default': 'Compute', 'required': True, 'optionsSql': 'SELECT name FROM categories ORDER BY 1'}

# the months of last year that the year being reported has already had: a fair thing to compare with
SAME_MONTHS = "substr(month, 6, 2) <= (SELECT max(substr(month, 6, 2)) FROM sales WHERE year = :year)"

reports = {}

# ---- 1. Executive dashboard ---------------------------------------------------------------------------------------
reports['Executive Dashboard'] = report(
    'Executive Dashboard', 'Executive Sales Dashboard', 'Revenue, orders, customers and margin at a glance, with the trend by month and the split by region, category, country and segment. Click a region or a category to open its report.',
    [YEAR],
    [
        {'name': 'kpi', 'sql': ("SELECT round(sum(CASE WHEN year = :year THEN revenue END)) AS revenue, "
                                "round(sum(CASE WHEN year = :year - 1 AND " + SAME_MONTHS + " THEN revenue END)) AS prev_revenue, "
                                "count(DISTINCT CASE WHEN year = :year THEN order_id END) AS orders, "
                                "count(DISTINCT CASE WHEN year = :year - 1 AND " + SAME_MONTHS + " THEN order_id END) AS prev_orders, "
                                "count(DISTINCT CASE WHEN year = :year THEN customer_id END) AS customers, "
                                "round(sum(CASE WHEN year = :year THEN revenue END) / count(DISTINCT CASE WHEN year = :year THEN order_id END)) AS avg_order, "
                                "round(100.0 * sum(CASE WHEN year = :year THEN revenue - cost END) / sum(CASE WHEN year = :year THEN revenue END), 1) AS margin, "
                                "round(100.0 * sum(CASE WHEN year = :year - 1 AND " + SAME_MONTHS + " THEN revenue - cost END) / sum(CASE WHEN year = :year - 1 AND " + SAME_MONTHS + " THEN revenue END), 1) AS prev_margin "
                                "FROM sales WHERE year IN (:year, :year - 1)")},
        {'name': 'monthly', 'sql': 'SELECT month, round(sum(revenue)) AS revenue, round(sum(revenue) - sum(cost)) AS profit FROM sales WHERE year = :year GROUP BY month ORDER BY month'},
        {'name': 'regions', 'sql': 'SELECT region, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY region ORDER BY revenue DESC'},
        {'name': 'categories', 'sql': 'SELECT category, round(sum(cost)) AS cost, round(sum(revenue) - sum(cost)) AS profit FROM sales WHERE year = :year GROUP BY category ORDER BY sum(revenue) DESC'},
        {'name': 'countries', 'sql': 'SELECT country, region, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY country ORDER BY revenue DESC LIMIT 10'},
        {'name': 'segments', 'sql': 'SELECT segment, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY segment ORDER BY revenue DESC'},
        {'name': 'regq', 'sql': 'SELECT region, quarter, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY region, quarter ORDER BY region, quarter'},
    ],
    [
        *title_bar('Executive Sales Dashboard', 'Fiscal year {{year}}  ·  US dollars  ·  cancelled orders excluded  ·  generated {{Format(Now, "yyyy-MM-dd")}}'),
        {'type': 'spacer', 'height': 4},
        tiles(
            tile('Revenue', '{{Format(First(revenue), "$#,##0")}}', delta('revenue', 'prev_revenue', 'vs last year'), 'kpi', BLUE),
            tile('Orders', '{{Format(First(orders), "#,##0")}}', delta('orders', 'prev_orders', 'vs last year'), 'kpi', ORANGE),
            tile('Customers', '{{First(customers)}}', 'who bought in {{year}}', 'kpi', GREEN),
            tile('Average order', '{{Format(First(avg_order), "$#,##0")}}', 'revenue per order', 'kpi', PURPLE),
            tile('Gross margin', '{{First(margin)}}%', 'last year  {{First(prev_margin)}}%', 'kpi', TEAL),
        ),
        {'type': 'spacer', 'height': 8},
        side_by_side(
            (3, [{'type': 'chart', 'chart': 'bar', 'dataset': 'monthly', 'category': 'month', 'title': 'Revenue and gross profit by month', 'height': 296, 'format': '$#,##0',
                  'series': [{'field': 'revenue', 'label': 'Revenue', 'color': BLUE}, {'field': 'profit', 'label': 'Gross profit', 'color': GREEN}]}]),
            (2, [{'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'regions', 'category': 'region', 'title': 'Revenue by region  (click to open the region)', 'height': 296, 'format': '$#,##0',
                  'series': [{'field': 'revenue', 'label': 'Revenue'}], 'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')}]),
        ),
        {'type': 'pageBreak'},
        side_by_side(
            (3, [{'type': 'chart', 'chart': 'bar', 'stacked': True, 'labels': True, 'dataset': 'categories', 'category': 'category', 'title': 'Cost and gross profit by category  (click to open the category)', 'height': 215, 'format': '$#,##0',
                  'series': [{'field': 'cost', 'label': 'Cost', 'color': '#b0bec5'}, {'field': 'profit', 'label': 'Gross profit', 'color': TEAL}],
                  'drill': drill('Product Performance', category='{{category}}', year='{{year}}')}]),
            (2, [{'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'segments', 'category': 'segment', 'title': 'Revenue by customer segment', 'height': 215, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}]}]),
        ),
        {'type': 'spacer', 'height': 6},
        side_by_side(
            (3, [{'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'countries', 'category': 'country', 'title': 'Top 10 countries by revenue', 'height': 235, 'format': '$#,##0',
                  'series': [{'field': 'revenue', 'label': 'Revenue', 'color': RED}], 'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')}]),
            (2, [{'type': 'matrix', 'dataset': 'regq', 'rows': ['region'], 'column': 'quarter', 'value': 'revenue', 'title': 'Revenue by region and quarter', 'format': '$#,##0', 'heat': 'blue',
                  'rowTotals': True, 'corner': 'Region', 'style': {'size': 8}, 'sortRows': 'total', 'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')}], True),
        ),
    ],
    landscape=True)

# ---- 2. Revenue by region -----------------------------------------------------------------------------------------
reports['Revenue by Region'] = report(
    'Revenue by Region', 'Revenue by Region and Country', 'Every region with its countries, customers, orders, revenue, share of the region and margin. Click a bar or a country to open its region.',
    [YEAR],
    [
        {'name': 'regions', 'sql': 'SELECT region, round(sum(revenue)) AS revenue, round(sum(revenue) - sum(cost)) AS profit FROM sales WHERE year = :year GROUP BY region ORDER BY revenue DESC'},
        {'name': 'countries', 'sql': 'SELECT region, country, count(DISTINCT customer_id) AS customers, count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue, round(sum(cost)) AS cost FROM sales WHERE year = :year GROUP BY country ORDER BY region, revenue DESC'},
    ],
    [
        *title_bar('Revenue by Region', '{{year}}  ·  {{Count()}} countries  ·  total {{Format(Sum(revenue), "$#,##0")}}', TEAL, 'countries'),
        {'type': 'spacer', 'height': 6},
        {'type': 'chart', 'chart': 'bar', 'dataset': 'regions', 'category': 'region', 'title': 'Revenue and gross profit by region', 'height': 200, 'format': '$#,##0',
         'series': [{'field': 'revenue', 'label': 'Revenue', 'color': TEAL}, {'field': 'profit', 'label': 'Gross profit', 'color': ORANGE}],
         'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')},
        {'type': 'table', 'dataset': 'countries', 'zebra': True, 'grandTotal': True,
         'groups': [{'field': 'region', 'header': '{{region}}   ·   {{Format(Sum(revenue), "$#,##0")}}', 'subtotals': True}],
         'columns': [
             {'field': 'country', 'header': 'Country', 'width': 2.2},
             {'field': 'customers', 'header': 'Customers', 'total': 'sum'},
             {'field': 'orders', 'header': 'Orders', 'total': 'sum', 'format': '#,##0'},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'total': 'sum', 'width': 1.4},
             {'expression': 'revenue / Sum(revenue)', 'header': 'Share of region', 'format': '0.0%'},
             {'expression': '(revenue - cost) / revenue', 'header': 'Margin', 'format': '0.0%'},
         ], 'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')},
    ])

# ---- 3. Top customers ---------------------------------------------------------------------------------------------
reports['Top Customers'] = report(
    'Top Customers', 'Top Customers', 'The customers that bought the most in a year, with where they are and what kind of customer they are. Click a customer to open their statement.',
    [YEAR, {'name': 'top', 'label': 'How many', 'type': 'integer', 'default': 15, 'required': True, 'options': [{'value': n} for n in (5, 10, 15, 25, 50)]}],
    [
        {'name': 'top', 'sql': 'SELECT row_number() OVER (ORDER BY sum(revenue) DESC) AS rank, customer_id, customer, country, segment, industry, count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY customer_id ORDER BY revenue DESC LIMIT :top'},
        {'name': 'segments', 'sql': 'SELECT segment, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY segment ORDER BY revenue DESC'},
        {'name': 'industries', 'sql': 'SELECT industry, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY industry ORDER BY revenue DESC LIMIT 8'},
    ],
    [
        *title_bar('Top {{top}} Customers of {{year}}', 'Together {{Format(Sum(revenue), "$#,##0")}} from {{Sum(orders)}} orders', PURPLE, 'top'),
        {'type': 'spacer', 'height': 6},
        {'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'top', 'category': 'customer', 'title': 'Revenue by customer  (click a bar for the statement)', 'height': 300, 'format': '$#,##0',
         'series': [{'field': 'revenue', 'label': 'Revenue', 'color': PURPLE}], 'drill': drill('Customer Statement', customer='{{customer_id}}')},
        {'type': 'table', 'dataset': 'top', 'zebra': True, 'grandTotal': True,
         'columns': [
             {'field': 'rank', 'header': '#', 'width': 0.45, 'align': 'center'},
             {'field': 'customer', 'header': 'Customer', 'width': 3},
             {'field': 'country', 'header': 'Country', 'width': 1.5},
             {'field': 'segment', 'header': 'Segment', 'width': 1.3},
             {'field': 'orders', 'header': 'Orders', 'width': 0.8, 'total': 'sum'},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'width': 1.3, 'total': 'sum'},
         ], 'drill': drill('Customer Statement', customer='{{customer_id}}')},
        {'type': 'spacer', 'height': 8},
        side_by_side(
            (1, [{'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'segments', 'category': 'segment', 'title': 'All customers: by segment', 'height': 210, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}]}]),
            (1, [{'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'industries', 'category': 'industry', 'title': 'All customers: by industry', 'height': 210, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': ORANGE}]}]),
        ),
    ])

# ---- 4. Product performance ---------------------------------------------------------------------------------------
BY_CATEGORY = ' AND (:category IS NULL OR category = :category)'
reports['Product Performance'] = report(
    'Product Performance', 'Product Performance', 'Units, revenue and margin of every product, by category. Leave the category empty for all of them.',
    [YEAR, {'name': 'category', 'label': 'Category', 'type': 'text', 'optionsSql': 'SELECT name FROM categories ORDER BY 1'}],
    [
        {'name': 'products', 'sql': 'SELECT category, product, sum(quantity) AS units, round(sum(revenue)) AS revenue, round(sum(cost)) AS cost FROM sales WHERE year = :year' + BY_CATEGORY + ' GROUP BY product_id ORDER BY category, revenue DESC'},
        {'name': 'best', 'sql': 'SELECT product, round(sum(revenue)) AS revenue FROM sales WHERE year = :year' + BY_CATEGORY + ' GROUP BY product_id ORDER BY revenue DESC LIMIT 10'},
        {'name': 'quarters', 'sql': "SELECT quarter, " + ', '.join(f"round(coalesce(sum(CASE WHEN category = '{c}' THEN revenue END), 0)) AS {c}" for c in ('Compute', 'Networking', 'Storage', 'Software', 'Services', 'Accessories')) + ' FROM sales WHERE year = :year' + BY_CATEGORY + ' GROUP BY quarter ORDER BY quarter'},
    ],
    [
        *title_bar('Product Performance {{year}}', '{{Coalesce(category, "All categories")}}  ·  {{Format(Sum(units), "#,##0")}} units sold, {{Format(Sum(revenue), "$#,##0")}} revenue, {{Format((Sum(revenue) - Sum(cost)) / Sum(revenue), "0.0%")}} gross margin', ORANGE, 'products'),
        {'type': 'spacer', 'height': 6},
        side_by_side(
            (1, [{'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'best', 'category': 'product', 'title': 'The ten best sellers by revenue', 'height': 270, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': ORANGE}]}]),
            (1, [{'type': 'chart', 'chart': 'bar', 'stacked': True, 'dataset': 'quarters', 'category': 'quarter', 'title': 'Revenue by quarter and category', 'height': 270, 'format': '$#,##0', 'series': [{'field': c} for c in ('Compute', 'Networking', 'Storage', 'Software', 'Services', 'Accessories')]}]),
        ),
        {'type': 'table', 'dataset': 'products', 'zebra': True, 'grandTotal': True,
         'groups': [{'field': 'category', 'header': '{{category}}', 'subtotals': True}],
         'columns': [
             {'field': 'product', 'header': 'Product', 'width': 3.2},
             {'field': 'units', 'header': 'Units', 'format': '#,##0', 'total': 'sum', 'width': 0.9},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'total': 'sum', 'width': 1.3},
             {'expression': 'revenue - cost', 'header': 'Gross profit', 'format': money, 'total': 'sum', 'width': 1.3},
             {'expression': '(revenue - cost) / revenue', 'header': 'Margin', 'format': '0.0%', 'width': 0.9},
         ]},
    ])

# ---- 5. Customer statement ----------------------------------------------------------------------------------------
reports['Customer Statement'] = report(
    'Customer Statement', 'Customer Statement', 'Everything one customer ordered: details, a chart of their spending and every order with its lines.',
    [{'name': 'customer', 'label': 'Customer', 'type': 'integer', 'default': 62, 'required': True, 'optionsSql': 'SELECT id, name FROM customers ORDER BY name'}],
    [
        {'name': 'info', 'sql': "SELECT c.name, c.city, c.country, c.region, c.segment, c.industry, c.customer_since, c.contact, c.credit_limit, e.name AS rep FROM customers c LEFT JOIN employees e ON e.id = c.rep_id WHERE c.id = :customer"},
        {'name': 'kpi', 'sql': 'SELECT count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue, round(sum(revenue) / count(DISTINCT order_id)) AS avg_order, min(order_date) AS first_order, max(order_date) AS last_order FROM sales WHERE customer_id = :customer'},
        {'name': 'lines', 'sql': "SELECT o.id AS order_id, o.order_date, o.status, o.channel, p.name AS product, i.quantity, i.unit_price, round(i.quantity * i.unit_price * (1 - o.discount), 2) AS amount FROM orders o JOIN order_items i ON i.order_id = o.id JOIN products p ON p.id = i.product_id WHERE o.customer_id = :customer ORDER BY o.order_date, o.id, p.name"},
        {'name': 'monthly', 'sql': 'SELECT month, round(sum(revenue)) AS revenue FROM sales WHERE customer_id = :customer GROUP BY month ORDER BY month'},
    ],
    [
        *title_bar('Customer Statement', '{{First(name)}}', GREEN, 'info'),
        rich('[size=11][b]{{First(city)}}, {{First(country)}}[/b]  ({{First(region)}})[/size]\n[size=9][color=' + MUTED + ']{{First(segment)}} customer in {{First(industry)}} since {{First(customer_since)}}   ·   contact {{First(contact)}}   ·   account manager {{First(rep)}}   ·   credit limit {{Format(First(credit_limit), "$#,##0")}}[/color][/size]', 'info'),
        {'type': 'spacer', 'height': 6},
        tiles(
            tile('Ordered', '{{Format(First(revenue), "$#,##0")}}', 'cancelled orders excluded', 'kpi', GREEN, 16),
            tile('Orders', '{{First(orders)}}', 'average {{Format(First(avg_order), "$#,##0")}}', 'kpi', BLUE, 16),
            tile('First order', '{{First(first_order)}}', 'on record', 'kpi', ORANGE, 16),
            tile('Last order', '{{First(last_order)}}', 'most recent', 'kpi', PURPLE, 16),
        ),
        {'type': 'spacer', 'height': 8},
        {'type': 'chart', 'chart': 'bar', 'dataset': 'monthly', 'category': 'month', 'title': 'Spending by month (cancelled orders excluded)', 'height': 180, 'labels': True, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': GREEN}]},
        {'type': 'table', 'dataset': 'lines', 'grandTotal': True,
         'groups': [{'field': 'order_id', 'header': 'Order {{order_id}}   ·   {{First(order_date)}}   ·   {{First(status)}}   ·   {{First(channel)}}', 'subtotals': True}],
         'columns': [
             {'field': 'product', 'header': 'Product', 'width': 3.4},
             {'field': 'quantity', 'header': 'Qty', 'width': 0.6, 'total': 'sum', 'format': '#,##0'},
             {'field': 'unit_price', 'header': 'Unit price', 'format': '#,##0.00', 'width': 1.1},
             {'field': 'amount', 'header': 'Amount', 'format': '#,##0.00', 'total': 'sum', 'width': 1.1},
         ]},
    ])

# ---- 6. Sales team leaderboard ------------------------------------------------------------------------------------
reports['Sales Team Leaderboard'] = report(
    'Sales Team Leaderboard', 'Sales Team Leaderboard', 'Who sold what, against the annual quota.',
    [YEAR],
    [
        {'name': 'reps', 'sql': 'SELECT e.name AS rep, e.title, e.region, e.annual_quota AS quota, count(DISTINCT s.order_id) AS orders, round(coalesce(sum(s.revenue), 0)) AS revenue FROM employees e LEFT JOIN sales s ON s.rep = e.name AND s.year = :year GROUP BY e.id ORDER BY revenue DESC'},
        {'name': 'channels', 'sql': 'SELECT channel, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY channel ORDER BY revenue DESC'},
    ],
    [
        *title_bar('Sales Team Leaderboard {{year}}', 'Annual quotas; a year in progress is measured against the whole year.   {{Count()}} people, {{Format(Sum(revenue) / Sum(quota), "0%")}} of the combined quota reached.', RED, 'reps'),
        {'type': 'spacer', 'height': 6},
        {'type': 'chart', 'chart': 'bar', 'dataset': 'reps', 'category': 'rep', 'title': 'Revenue against quota', 'height': 240, 'format': '$#,##0',
         'series': [{'field': 'revenue', 'label': 'Revenue', 'color': RED}, {'field': 'quota', 'label': 'Annual quota', 'color': '#b0bec5'}]},
        {'type': 'table', 'dataset': 'reps', 'zebra': True, 'grandTotal': True,
         'columns': [
             {'field': 'rep', 'header': 'Account executive', 'width': 2.2},
             {'field': 'region', 'header': 'Region', 'width': 1.8},
             {'field': 'orders', 'header': 'Orders', 'width': 0.8, 'total': 'sum'},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'width': 1.3, 'total': 'sum'},
             {'field': 'quota', 'header': 'Annual quota', 'format': money, 'width': 1.3, 'total': 'sum'},
             {'expression': 'revenue / quota', 'header': 'Of quota', 'format': '0%', 'width': 0.9},
         ]},
        {'type': 'spacer', 'height': 8},
        {'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'channels', 'category': 'channel', 'title': 'Revenue by sales channel', 'height': 210, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}]},
    ])

# ---- 7. Region snapshot: a report on its own, and what Regional Review repeats ---------------------------------------
IN_REGION = ' WHERE year = :year AND region = :region'
reports['Region Snapshot'] = report(
    'Region Snapshot', 'Region Snapshot', 'One region in a year: figures, trend, mix by category and country, and its best customers. Click a category row for its products, a customer for the statement.',
    [YEAR, REGION],
    [
        {'name': 'kpi', 'sql': ("SELECT round(sum(CASE WHEN year = :year THEN revenue END)) AS revenue, round(sum(CASE WHEN year = :year - 1 AND " + SAME_MONTHS + " THEN revenue END)) AS prev_revenue, "
                                "count(DISTINCT CASE WHEN year = :year THEN order_id END) AS orders, count(DISTINCT CASE WHEN year = :year THEN customer_id END) AS customers, "
                                "round(100.0 * sum(CASE WHEN year = :year THEN revenue - cost END) / sum(CASE WHEN year = :year THEN revenue END), 1) AS margin, "
                                "count(DISTINCT CASE WHEN year = :year THEN country END) AS countries FROM sales WHERE region = :region AND year IN (:year, :year - 1)")},
        {'name': 'monthly', 'sql': 'SELECT month, round(sum(revenue)) AS revenue FROM sales' + IN_REGION + ' GROUP BY month ORDER BY month'},
        {'name': 'cats', 'sql': 'SELECT category, round(sum(revenue)) AS revenue FROM sales' + IN_REGION + ' GROUP BY category ORDER BY revenue DESC'},
        {'name': 'countries', 'sql': 'SELECT country, round(sum(revenue)) AS revenue FROM sales' + IN_REGION + ' GROUP BY country ORDER BY revenue DESC LIMIT 8'},
        {'name': 'catq', 'sql': 'SELECT category, quarter, round(sum(revenue)) AS revenue FROM sales' + IN_REGION + ' GROUP BY category, quarter ORDER BY category, quarter'},
        {'name': 'top', 'sql': 'SELECT customer_id, customer, country, segment, count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue FROM sales' + IN_REGION + ' GROUP BY customer_id ORDER BY revenue DESC LIMIT 8'},
    ],
    [
        *title_bar('{{region}}', 'Regional snapshot  ·  {{year}}  ·  {{First(countries)}} countries', BLUE, 'kpi'),
        {'type': 'spacer', 'height': 4},
        tiles(
            tile('Revenue', '{{Format(First(revenue), "$#,##0")}}', delta('revenue', 'prev_revenue', 'vs last year'), 'kpi', BLUE, 16),
            tile('Orders', '{{Format(First(orders), "#,##0")}}', 'in {{year}}', 'kpi', ORANGE, 16),
            tile('Customers', '{{First(customers)}}', 'who bought', 'kpi', GREEN, 16),
            tile('Gross margin', '{{First(margin)}}%', 'of revenue', 'kpi', TEAL, 16),
        ),
        {'type': 'spacer', 'height': 8},
        {'type': 'chart', 'chart': 'area', 'dataset': 'monthly', 'category': 'month', 'title': 'Revenue by month', 'height': 150, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': BLUE}]},
        side_by_side(
            (1, [{'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'cats', 'category': 'category', 'title': 'By category', 'height': 175, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}],
                  'drill': drill('Product Performance', category='{{category}}', year='{{year}}')}]),
            (1, [{'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'countries', 'category': 'country', 'title': 'By country', 'height': 175, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': TEAL}]}]),
        ),
        {'type': 'matrix', 'dataset': 'catq', 'rows': ['category'], 'column': 'quarter', 'value': 'revenue', 'title': 'Revenue by category and quarter', 'format': '$#,##0', 'heat': 'blue',
         'rowTotals': True, 'columnTotals': True, 'corner': 'Category', 'sortRows': 'total', 'drill': drill('Product Performance', category='{{category}}', year='{{year}}')},
        {'type': 'table', 'dataset': 'top', 'zebra': True,
         'columns': [
             {'field': 'customer', 'header': 'Best customers', 'width': 3.2},
             {'field': 'country', 'header': 'Country', 'width': 1.5},
             {'field': 'segment', 'header': 'Segment', 'width': 1.3},
             {'field': 'orders', 'header': 'Orders', 'width': 0.7},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'width': 1.2},
         ], 'drill': drill('Customer Statement', customer='{{customer_id}}')},
    ])

# ---- 8. Regional review: a cover, then Region Snapshot once for each region (sub-reports) -----------------------------
reports['Regional Review'] = report(
    'Regional Review', 'Regional Review', 'A cover and one page per region: the Region Snapshot report, run once for every region as a sub-report.',
    [YEAR],
    [
        {'name': 'regions', 'sql': 'SELECT region, count(DISTINCT customer_id) AS customers, count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY region ORDER BY revenue DESC'},
    ],
    [
        {'type': 'image', 'src': BANNER, 'fitWidth': True},
        {'type': 'spacer', 'height': 10},
        rich('[size=28][b][color=' + NAVY + ']Regional Review {{year}}[/color][/b][/size]\n[size=11][color=' + MUTED + ']Aurelia Systems  ·  sales by region  ·  generated {{Format(Now, "yyyy-MM-dd")}}[/color][/size]'),
        {'type': 'spacer', 'height': 10},
        rich('This review covers [b]{{Count()}} regions[/b] and [b]{{Format(Sum(revenue), "$#,##0")}}[/b] of revenue from [b]{{Format(Sum(orders), "#,##0")}} orders[/b].\n'
             '- Each region has a page of its own: the [b]Region Snapshot[/b] report, run as a sub-report with that region and this year.\n'
             '- In the table below, a click on a region opens its snapshot on its own.\n'
             '- The same pages can be made for any report: a sub-report takes parameters from a row, or from the page it sits on.', 'regions', size=10),
        {'type': 'spacer', 'height': 8},
        {'type': 'table', 'dataset': 'regions', 'zebra': True, 'grandTotal': True,
         'columns': [
             {'field': 'region', 'header': 'Region', 'width': 2.5},
             {'field': 'customers', 'header': 'Customers', 'total': 'sum'},
             {'field': 'orders', 'header': 'Orders', 'format': '#,##0', 'total': 'sum'},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'total': 'sum', 'width': 1.4},
             {'expression': 'revenue / Sum(revenue)', 'header': 'Share', 'format': '0.0%'},
         ], 'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')},
        {'type': 'pageBreak'},
        {'type': 'subreport', 'report': 'Region Snapshot', 'dataset': 'regions', 'params': {'region': '{{region}}'}, 'pageBreak': True, 'limit': 8},
    ])

# ---- 9. Category card: icon from the database next to the category's figures --------------------------------------------
reports['Category Card'] = report(
    'Category Card', 'Category Card', 'One product category: its picture (read from the database), figures and best sellers.',
    [YEAR, CATEGORY],
    [
        {'name': 'cat', 'sql': ("SELECT c.name, c.tagline, c.icon, c.color, coalesce(round(sum(s.revenue)), 0) AS revenue, coalesce(sum(s.quantity), 0) AS units, count(DISTINCT s.customer_id) AS customers, "
                                "coalesce(round(100.0 * (sum(s.revenue) - sum(s.cost)) / sum(s.revenue), 1), 0) AS margin "
                                "FROM categories c LEFT JOIN sales s ON s.category = c.name AND s.year = :year WHERE c.name = :category GROUP BY c.name")},
        {'name': 'products', 'sql': 'SELECT product, sum(quantity) AS units, round(sum(revenue)) AS revenue FROM sales WHERE year = :year AND category = :category GROUP BY product_id ORDER BY revenue DESC LIMIT 5'},
    ],
    [
        {'type': 'columns', 'gap': 10, 'columns': [
            {'width': 1, 'card': True, 'accent': '#4e79a7', 'items': [{'type': 'image', 'src': '{{First(icon)}}', 'dataset': 'cat', 'width': 64, 'align': 'center'}]},
            {'width': 5, 'card': True, 'items': [
                rich('[size=16][b]{{First(name)}}[/b][/size]  [size=9][color=' + MUTED + ']{{year}}[/color][/size]\n[color=' + MUTED + ']{{First(tagline)}}[/color]\n'
                     '[size=9]Revenue [b]{{Format(First(revenue), "$#,##0")}}[/b]     Units [b]{{Format(First(units), "#,##0")}}[/b]     Customers [b]{{First(customers)}}[/b]     Margin [b]{{First(margin)}}%[/b][/size]', 'cat')]},
            {'width': 5, 'items': [{'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'products', 'category': 'product', 'title': 'Best sellers', 'height': 112, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': '#4e79a7'}]}]},
        ]},
        {'type': 'spacer', 'height': 6},
    ])

# ---- 10. Product catalog: Category Card once for each category -----------------------------------------------------------
reports['Product Catalog'] = report(
    'Product Catalog', 'Product Catalog', 'Every product category as a card with its picture, read from the database: the Category Card report, run once for each category as a sub-report.',
    [YEAR],
    [
        {'name': 'categories', 'sql': 'SELECT name AS category FROM categories ORDER BY name'},
        {'name': 'company', 'sql': 'SELECT name, tagline, logo FROM company'},
    ],
    [
        *title_bar('Product Catalog {{year}}', '{{First(name)}}  ·  {{First(tagline)}}', NAVY, 'company'),
        rich('[size=10]Six categories, each laid out by the same report. The [b]picture[/b] of a category is a column of the database, shown by an [u]image item[/u] that reads it; the [b]cards[/b] are columns with a border; the text is [color=#e15759]rich[/color].[/size]'),
        {'type': 'spacer', 'height': 8},
        {'type': 'subreport', 'report': 'Category Card', 'dataset': 'categories', 'params': {'category': '{{category}}'}},
    ])

# ---- 11. Sales matrix: cross-tabs with color scales ----------------------------------------------------------------------
reports['Sales Matrix'] = report(
    'Sales Matrix', 'Sales Matrix', 'Cross-tab tables with color scales, in the manner of Tableau\'s highlight tables. Click a column heading to sort the rows by it; click a cell to drill down.',
    [YEAR],
    [
        {'name': 'catq', 'sql': 'SELECT category, quarter, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY category, quarter ORDER BY category, quarter'},
        {'name': 'geo', 'sql': 'SELECT region, country, quarter, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY region, country, quarter ORDER BY region, country, quarter'},
        {'name': 'margin', 'sql': 'SELECT segment, category, round(100.0 * (sum(revenue) - sum(cost)) / sum(revenue), 1) AS margin FROM sales WHERE year = :year GROUP BY segment, category ORDER BY segment, category'},
    ],
    [
        *title_bar('Sales Matrix {{year}}', 'Cross-tabs: one field across the top, one or two down the side, one number in the cells', '#8e24aa'),
        {'type': 'spacer', 'height': 4},
        {'type': 'matrix', 'dataset': 'catq', 'rows': ['category'], 'column': 'quarter', 'value': 'revenue', 'title': 'Revenue by category and quarter  (click a cell for the category\'s products)', 'format': '$#,##0',
         'heat': 'blue', 'rowTotals': True, 'columnTotals': True, 'corner': 'Category', 'sortRows': 'total', 'drill': drill('Product Performance', category='{{category}}', year='{{year}}')},
        {'type': 'spacer', 'height': 6},
        {'type': 'matrix', 'dataset': 'margin', 'rows': ['segment'], 'column': 'category', 'value': 'margin', 'agg': 'avg', 'title': 'Gross margin % by customer segment and category  (diverging: below and above the middle of the range)',
         'format': '0.0', 'heat': 'diverging', 'rowTotals': False, 'corner': 'Segment'},
        {'type': 'spacer', 'height': 6},
        {'type': 'matrix', 'dataset': 'geo', 'rows': ['region', 'country'], 'column': 'quarter', 'value': 'revenue', 'title': 'Revenue by region, country and quarter  (two row fields, with subtotals)', 'format': '$#,##0',
         'heat': 'orange', 'rowTotals': True, 'columnTotals': True, 'subtotals': True, 'corner': 'Region / country', 'sortRows': 'total',
         'drill': drill('Region Snapshot', region='{{region}}', year='{{year}}')},
    ],
    landscape=True)

for name, r in reports.items():
    with open(os.path.join(OUT, name + '.json'), 'w', encoding='utf-8') as f:
        json.dump(r, f, indent=2, ensure_ascii=False)
        f.write('\n')
print(f'{len(reports)} reports in {OUT}')
