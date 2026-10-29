#!/usr/bin/env python3
"""Writes the demo's report definitions into sales_demo.sqldb.reports/ (one JSON file each), where the sql tool looks for
them: `sql demo/sales_demo.sqldb --listen 8080` then offers them on its Reports tab and at /reports.

    python3 make_reports.py
"""
import json
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sales_demo.sqldb.reports')
os.makedirs(OUT, exist_ok=True)

NAVY, BLUE, TEAL, ORANGE, GREEN, PURPLE, RED, GREY = '#0d47a1', '#1976d2', '#00897b', '#fb8c00', '#43a047', '#8e24aa', '#e53935', '#546e7a'


def banner(text, color=NAVY, size=22):
    return {'type': 'text', 'text': ' ' + text, 'style': {'face': 'bold', 'size': size, 'color': '#ffffff', 'background': color}}


def heading(text, color=NAVY):
    return {'type': 'text', 'text': text, 'style': {'face': 'bold', 'size': 13, 'color': color}}


def note(text, dataset=None, **style):
    item = {'type': 'text', 'text': text, 'style': {'size': 10, 'color': GREY, **style}}
    if dataset:
        item['dataset'] = dataset
    return item


def kpis(text, dataset, color='#e3f2fd'):
    return {'type': 'text', 'text': ' ' + text, 'dataset': dataset, 'style': {'face': 'bold', 'size': 12, 'color': NAVY, 'background': color}}


def report(name, title, description, parameters, datasets, body, landscape=False):
    return {
        'format': 1, 'name': name, 'title': title, 'description': description,
        'page': {'size': 'A4', 'orientation': 'landscape' if landscape else 'portrait', 'margins': {'top': 44, 'right': 36, 'bottom': 40, 'left': 36}},
        'parameters': parameters, 'datasets': datasets,
        'header': [{'type': 'text', 'text': '{{ReportName}}', 'style': {'face': 'bold', 'size': 9, 'color': GREY}}, {'type': 'rule'}],
        'footer': [{'type': 'rule'}, {'type': 'text', 'text': 'Fictional demo data   ·   Page {{PageNumber}} of {{PageCount}}', 'style': {'size': 8, 'color': GREY, 'align': 'right'}}],
        'body': body,
    }


YEAR = {'name': 'year', 'label': 'Year', 'type': 'integer', 'default': 2026, 'required': True, 'optionsSql': 'SELECT DISTINCT year FROM sales ORDER BY 1 DESC'}
money = '$#,##0'

reports = {}

# ---- 1. Executive dashboard ---------------------------------------------------------------------------------------
reports['Executive Dashboard'] = report(
    'Executive Dashboard', 'Executive Sales Dashboard', 'Revenue, orders, customers and margin at a glance, with the trend by month and the split by region, category, country and segment.',
    [YEAR],
    [
        {'name': 'kpi', 'sql': "SELECT round(sum(revenue)) AS revenue, count(DISTINCT order_id) AS orders, count(DISTINCT customer_id) AS customers, round(sum(revenue) / count(DISTINCT order_id)) AS avg_order, round(100.0 * (sum(revenue) - sum(cost)) / sum(revenue), 1) AS margin FROM sales WHERE year = :year"},
        {'name': 'monthly', 'sql': 'SELECT month, round(sum(revenue)) AS revenue, round(sum(revenue) - sum(cost)) AS profit FROM sales WHERE year = :year GROUP BY month ORDER BY month'},
        {'name': 'regions', 'sql': 'SELECT region, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY region ORDER BY revenue DESC'},
        {'name': 'categories', 'sql': 'SELECT category, round(sum(cost)) AS cost, round(sum(revenue) - sum(cost)) AS profit FROM sales WHERE year = :year GROUP BY category ORDER BY sum(revenue) DESC'},
        {'name': 'countries', 'sql': 'SELECT country, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY country ORDER BY revenue DESC LIMIT 10'},
        {'name': 'segments', 'sql': 'SELECT segment, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY segment ORDER BY revenue DESC'},
    ],
    [
        banner('Executive Sales Dashboard', size=26),
        note('Fiscal year {{year}}   ·   all amounts in US dollars   ·   cancelled orders excluded   ·   generated {{Format(Now, "yyyy-MM-dd")}}'),
        {'type': 'spacer', 'height': 6},
        kpis('Revenue  {{Format(First(revenue), "$#,##0")}}      Orders  {{Format(First(orders), "#,##0")}}      Customers  {{First(customers)}}      Average order  {{Format(First(avg_order), "$#,##0")}}      Gross margin  {{First(margin)}}%', 'kpi'),
        {'type': 'spacer', 'height': 10},
        {'type': 'chart', 'chart': 'bar', 'dataset': 'monthly', 'category': 'month', 'title': 'Revenue and gross profit by month', 'height': 210, 'format': '$#,##0',
         'series': [{'field': 'revenue', 'label': 'Revenue', 'color': BLUE}, {'field': 'profit', 'label': 'Gross profit', 'color': GREEN}]},
        {'type': 'chart', 'chart': 'area', 'dataset': 'monthly', 'category': 'month', 'title': 'Revenue trend', 'height': 170, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': ORANGE}]},
        {'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'regions', 'category': 'region', 'title': 'Revenue by region', 'height': 220, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}]},
        {'type': 'chart', 'chart': 'bar', 'stacked': True, 'labels': True, 'dataset': 'categories', 'category': 'category', 'title': 'What a dollar of revenue is made of, by product category (cost and gross profit)', 'height': 230, 'format': '$#,##0',
         'series': [{'field': 'cost', 'label': 'Cost', 'color': '#b0bec5'}, {'field': 'profit', 'label': 'Gross profit', 'color': TEAL}]},
        {'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'countries', 'category': 'country', 'title': 'Top 10 countries by revenue', 'height': 250, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': RED}]},
        {'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'segments', 'category': 'segment', 'title': 'Revenue by customer segment', 'height': 230, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}]},
    ],
    landscape=True)

# ---- 2. Revenue by region -----------------------------------------------------------------------------------------
reports['Revenue by Region'] = report(
    'Revenue by Region', 'Revenue by Region and Country', 'Every region with its countries, customers, orders, revenue, share of the region and margin.',
    [YEAR],
    [
        {'name': 'regions', 'sql': 'SELECT region, round(sum(revenue)) AS revenue, round(sum(revenue) - sum(cost)) AS profit FROM sales WHERE year = :year GROUP BY region ORDER BY revenue DESC'},
        {'name': 'countries', 'sql': 'SELECT region, country, count(DISTINCT customer_id) AS customers, count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue, round(sum(cost)) AS cost FROM sales WHERE year = :year GROUP BY country ORDER BY region, revenue DESC'},
    ],
    [
        banner('Revenue by Region', TEAL),
        note('{{year}}   ·   {{Count()}} countries   ·   total {{Format(Sum(revenue), "$#,##0")}}', 'countries'),
        {'type': 'spacer', 'height': 6},
        {'type': 'chart', 'chart': 'bar', 'dataset': 'regions', 'category': 'region', 'title': 'Revenue and gross profit by region', 'height': 200, 'format': '$#,##0',
         'series': [{'field': 'revenue', 'label': 'Revenue', 'color': TEAL}, {'field': 'profit', 'label': 'Gross profit', 'color': ORANGE}]},
        {'type': 'table', 'dataset': 'countries', 'zebra': True, 'grandTotal': True,
         'groups': [{'field': 'region', 'header': '{{region}}   ·   {{Format(Sum(revenue), "$#,##0")}}', 'subtotals': True}],
         'columns': [
             {'field': 'country', 'header': 'Country', 'width': 2.2},
             {'field': 'customers', 'header': 'Customers', 'total': 'sum'},
             {'field': 'orders', 'header': 'Orders', 'total': 'sum', 'format': '#,##0'},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'total': 'sum', 'width': 1.4},
             {'expression': 'revenue / Sum(revenue)', 'header': 'Share of region', 'format': '0.0%'},
             {'expression': '(revenue - cost) / revenue', 'header': 'Margin', 'format': '0.0%'},
         ]},
    ])

# ---- 3. Top customers ---------------------------------------------------------------------------------------------
reports['Top Customers'] = report(
    'Top Customers', 'Top Customers', 'The customers that bought the most in a year, with where they are and what kind of customer they are.',
    [YEAR, {'name': 'top', 'label': 'How many', 'type': 'integer', 'default': 15, 'required': True, 'options': [{'value': n} for n in (5, 10, 15, 25, 50)]}],
    [
        {'name': 'top', 'sql': 'SELECT row_number() OVER (ORDER BY sum(revenue) DESC) AS rank, customer, country, segment, industry, count(DISTINCT order_id) AS orders, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY customer_id ORDER BY revenue DESC LIMIT :top'},
        {'name': 'segments', 'sql': 'SELECT segment, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY segment ORDER BY revenue DESC'},
        {'name': 'industries', 'sql': 'SELECT industry, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY industry ORDER BY revenue DESC LIMIT 8'},
    ],
    [
        banner('Top {{top}} Customers of {{year}}', PURPLE),
        note('Together {{Format(Sum(revenue), "$#,##0")}} from {{Sum(orders)}} orders', 'top'),
        {'type': 'spacer', 'height': 6},
        {'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'top', 'category': 'customer', 'title': 'Revenue by customer', 'height': 300, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': PURPLE}]},
        {'type': 'table', 'dataset': 'top', 'zebra': True, 'grandTotal': True,
         'columns': [
             {'field': 'rank', 'header': '#', 'width': 0.45, 'align': 'center'},
             {'field': 'customer', 'header': 'Customer', 'width': 3},
             {'field': 'country', 'header': 'Country', 'width': 1.5},
             {'field': 'segment', 'header': 'Segment', 'width': 1.3},
             {'field': 'orders', 'header': 'Orders', 'width': 0.8, 'total': 'sum'},
             {'field': 'revenue', 'header': 'Revenue', 'format': money, 'width': 1.3, 'total': 'sum'},
         ]},
        {'type': 'spacer', 'height': 8},
        {'type': 'chart', 'chart': 'pie', 'donut': True, 'dataset': 'segments', 'category': 'segment', 'title': 'All customers: revenue by segment', 'height': 200, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue'}]},
        {'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'industries', 'category': 'industry', 'title': 'All customers: revenue by industry', 'height': 220, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': ORANGE}]},
    ])

# ---- 4. Product performance ---------------------------------------------------------------------------------------
reports['Product Performance'] = report(
    'Product Performance', 'Product Performance', 'Units, revenue and margin of every product, by category.',
    [YEAR],
    [
        {'name': 'products', 'sql': 'SELECT category, product, sum(quantity) AS units, round(sum(revenue)) AS revenue, round(sum(cost)) AS cost FROM sales WHERE year = :year GROUP BY product_id ORDER BY category, revenue DESC'},
        {'name': 'best', 'sql': 'SELECT product, round(sum(revenue)) AS revenue FROM sales WHERE year = :year GROUP BY product_id ORDER BY revenue DESC LIMIT 10'},
        {'name': 'quarters', 'sql': "SELECT quarter, " + ', '.join(f"round(coalesce(sum(CASE WHEN category = '{c}' THEN revenue END), 0)) AS {c}" for c in ('Compute', 'Networking', 'Storage', 'Software', 'Services', 'Accessories')) + ' FROM sales WHERE year = :year GROUP BY quarter ORDER BY quarter'},
    ],
    [
        banner('Product Performance {{year}}', ORANGE),
        note('{{Format(Sum(units), "#,##0")}} units sold, {{Format(Sum(revenue), "$#,##0")}} revenue, {{Format((Sum(revenue) - Sum(cost)) / Sum(revenue), "0.0%")}} gross margin', 'products'),
        {'type': 'spacer', 'height': 6},
        {'type': 'chart', 'chart': 'bar', 'horizontal': True, 'labels': True, 'dataset': 'best', 'category': 'product', 'title': 'The ten best sellers by revenue', 'height': 290, 'format': '$#,##0', 'series': [{'field': 'revenue', 'label': 'Revenue', 'color': ORANGE}]},
        {'type': 'chart', 'chart': 'bar', 'stacked': True, 'dataset': 'quarters', 'category': 'quarter', 'title': 'Revenue by quarter and category', 'height': 240, 'format': '$#,##0', 'series': [{'field': c} for c in ('Compute', 'Networking', 'Storage', 'Software', 'Services', 'Accessories')]},
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
        {'name': 'lines', 'sql': "SELECT o.id AS order_id, o.order_date, o.status, o.channel, p.name AS product, i.quantity, i.unit_price, round(i.quantity * i.unit_price * (1 - o.discount), 2) AS amount FROM orders o JOIN order_items i ON i.order_id = o.id JOIN products p ON p.id = i.product_id WHERE o.customer_id = :customer ORDER BY o.order_date, o.id, p.name"},
        {'name': 'monthly', 'sql': 'SELECT month, round(sum(revenue)) AS revenue FROM sales WHERE customer_id = :customer GROUP BY month ORDER BY month'},
    ],
    [
        banner('Customer Statement', GREEN),
        {'type': 'text', 'text': '{{First(name)}}', 'dataset': 'info', 'style': {'face': 'bold', 'size': 18, 'color': NAVY}},
        {'type': 'text', 'text': '{{First(city)}}, {{First(country)}}  ({{First(region)}})', 'dataset': 'info', 'style': {'size': 11}},
        {'type': 'text', 'text': '{{First(segment)}} customer in {{First(industry)}} since {{First(customer_since)}}   ·   contact {{First(contact)}}   ·   account manager {{First(rep)}}   ·   credit limit {{Format(First(credit_limit), "$#,##0")}}', 'dataset': 'info', 'style': {'size': 10, 'color': GREY}},
        {'type': 'spacer', 'height': 8},
        kpis('{{Count()}} order lines   ·   {{Format(Sum(amount), "$#,##0.00")}} ordered (cancelled orders included in the lines below)', 'lines', '#e8f5e9'),
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
        banner('Sales Team Leaderboard {{year}}', RED),
        note('Annual quotas; a year in progress is measured against the whole year. {{Count()}} people, {{Format(Sum(revenue) / Sum(quota), "0%")}} of the combined quota reached.', 'reps'),
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

for name, r in reports.items():
    with open(os.path.join(OUT, name + '.json'), 'w', encoding='utf-8') as f:
        json.dump(r, f, indent=2, ensure_ascii=False)
        f.write('\n')
print(f'{len(reports)} reports in {OUT}')
