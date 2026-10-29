# Sales demo

A fictional B2B company that sells servers, networking, storage, software, services and accessories to customers around the world, with
reports on it for the `sql` tool's [reporting module](../docs/reports.md). Every company, person and figure is made up.

```
sql demo/sales_demo.sqldb --listen 8080
```

Open <http://127.0.0.1:8080/> and choose the **Reports** tab (or <http://127.0.0.1:8080/reports> for the customer view). The reports are in
`sales_demo.sqldb.reports/`, where the tool looks for them by default; **Report Designer** edits them. (The tool locks the database file: work
on a copy if you want to keep the original untouched.)

| Report | Shows |
| --- | --- |
| Executive Sales Dashboard | Revenue, orders, customers, average order and margin; revenue and profit by month, the trend (an area), revenue by region and by segment (donuts), cost and profit by category (stacked), the top countries (horizontal bars with values). A year parameter. Point at the charts, click a month, switch a series off. |
| Revenue by Region and Country | Each region with its countries, customers, orders, revenue, share of the region and margin; subtotals and a grand total. |
| Top Customers | The top N customers of a year (a parameter), as horizontal bars and a ranked table you can sort, with revenue by segment (a donut) and by industry. |
| Product Performance | Units, revenue, gross profit and margin per product, by category (fold a category to see its totals), the ten best sellers, and revenue by quarter stacked by category. |
| Customer Statement | One customer (choose it): details, spending by month, and every order with its lines and subtotals. |
| Sales Team Leaderboard | Each account executive's revenue against their annual quota, and revenue by sales channel. |

## The data

| Table | Rows | |
| --- | ---: | --- |
| `customers` | 150 | In 30 countries in five regions (names in Latin, Greek and Cyrillic script), seven segments (enterprise, mid-market, small business, startup, education, government, non-profit), twelve industries |
| `orders` | 378 | January 2025 to October 2026, with growth, seasonality, channels, discounts, and a status that follows the order's age (the newest are pending) |
| `order_items` | 1,105 | 1 to 6 lines per order |
| `products` | 35 | Six categories, from a $58 per-seat license to a $26,500 cloud migration |
| `employees` | 10 | Account executives with regions and annual quotas |
| `sales` (view) | | One row per order line that counts as sales, with everything a report groups by: year, month, quarter, customer, country, region, segment, rep, category, revenue and cost |

The database file is about 1 MB.

## Rebuilding

`generate.py` writes `sales_demo.sql` (always the same data: the seed is fixed), `build.sh` builds the database from it, and `make_reports.py`
writes the report definitions, which are plain JSON you can also edit by hand or in the designer.

```
./build.sh path/to/sql      # generate.py, then sql sales_demo.sqldb -f sales_demo.sql
python3 make_reports.py
```
