#!/usr/bin/env python3
"""Writes sales_demo.sql: a fictional B2B sales database (employees, customers, products, orders and their lines), the same
every time (fixed seed). Build the database with build.sh. Every company, person and figure is made up.

    python3 generate.py > sales_demo.sql
"""
import datetime
import math
import random

rnd = random.Random(2026)
TODAY = datetime.date(2026, 10, 29)  # the demo's "now": the newest orders are still pending

# ---- where customers are ---------------------------------------------------------------------------------------------
# region, country, cities, brand words, local business nouns, legal suffixes, first names, last names
PLACES = [
    ('North America', 'United States', ['Austin', 'Boston', 'Chicago', 'Denver', 'Seattle', 'Atlanta', 'Portland'],
     ['Summit', 'Beacon', 'Ironwood', 'Redwood', 'Lakeshore', 'Keystone', 'Pioneer', 'Harbor', 'Cedar', 'Bluebird'], ['Systems', 'Industries', 'Partners', 'Labs', 'Group'], ['Inc.', 'LLC', 'Corp.'],
     ['James', 'Emily', 'Michael', 'Sarah', 'David', 'Jessica', 'Daniel', 'Olivia'], ['Carter', 'Nguyen', 'Brooks', 'Patel', 'Reyes', 'Foster', 'Kim', 'Sullivan']),
    ('North America', 'Canada', ['Toronto', 'Vancouver', 'Calgary', 'Montréal', 'Ottawa'],
     ['Northern', 'Maple', 'Polar', 'Glacier', 'Timberline', 'Cascade', 'Laurentian'], ['Technologies', 'Resources', 'Logistics', 'Foods'], ['Ltd.', 'Inc.', 'Ltée'],
     ['Liam', 'Chloé', 'Noah', 'Amélie', 'Ethan', 'Sophie'], ['Tremblay', 'Gagnon', 'MacDonald', 'Singh', 'Roy', 'Wong']),
    ('North America', 'Mexico', ['Monterrey', 'Guadalajara', 'Puebla', 'Querétaro'],
     ['Azteca', 'Sierra', 'Dorado', 'Montaña', 'Sol', 'Maya', 'Aurora'], ['Industrial', 'Comercial', 'Tecnología', 'Logística'], ['S.A. de C.V.', 'S. de R.L.'],
     ['Carlos', 'Lucía', 'Diego', 'Valentina', 'Javier', 'Ximena'], ['Hernández', 'García', 'Ramírez', 'Morales', 'Ortega', 'Castillo']),
    ('Europe', 'Germany', ['Munich', 'Hamburg', 'Stuttgart', 'Düsseldorf', 'Leipzig', 'Köln'],
     ['Müller', 'Schneider', 'Brandt', 'Vogel', 'Hoffmann', 'Kessler', 'Lindner', 'Weiß'], ['Maschinenbau', 'Logistik', 'Elektronik', 'Anlagenbau', 'Technik'], ['GmbH', 'AG', 'KG'],
     ['Lukas', 'Anna', 'Felix', 'Laura', 'Jonas', 'Katharina'], ['Becker', 'Fischer', 'Wagner', 'Schäfer', 'Koch', 'Richter']),
    ('Europe', 'France', ['Lyon', 'Toulouse', 'Nantes', 'Lille', 'Bordeaux'],
     ['Lumière', 'Beaumont', 'Delacroix', 'Fontaine', 'Rivière', 'Montfort', 'Laurent'], ['Industries', 'Conseil', 'Énergie', 'Distribution'], ['SARL', 'SAS', 'SA'],
     ['Camille', 'Hugo', 'Léa', 'Arthur', 'Manon', 'Louis'], ['Martin', 'Bernard', 'Dubois', 'Moreau', 'Lefèvre', 'Girard']),
    ('Europe', 'United Kingdom', ['Manchester', 'Leeds', 'Bristol', 'Glasgow', 'Cardiff', 'Birmingham'],
     ['Albion', 'Thornbury', 'Kingsley', 'Ashford', 'Wellington', 'Harcourt', 'Fairfax'], ['Engineering', 'Holdings', 'Solutions', 'Retail'], ['Ltd', 'plc', 'LLP'],
     ['Oliver', 'Amelia', 'George', 'Isla', 'Harry', 'Poppy'], ['Smith', 'Taylor', 'Evans', 'Hughes', 'Walker', 'Bennett']),
    ('Europe', 'Italy', ['Milano', 'Torino', 'Bologna', 'Verona', 'Napoli'],
     ['Rossi', 'Bianchi', 'Ferrari', 'Conti', 'Marini', 'Gallo', 'Greco'], ['Meccanica', 'Costruzioni', 'Alimentari', 'Tessile'], ['S.p.A.', 'S.r.l.'],
     ['Marco', 'Giulia', 'Luca', 'Chiara', 'Matteo', 'Elena'], ['Romano', 'Colombo', 'Ricci', 'Bruno', 'Esposito', 'De Luca']),
    ('Europe', 'Spain', ['Barcelona', 'Valencia', 'Sevilla', 'Bilbao', 'Zaragoza'],
     ['Iberia', 'Costa', 'Alhambra', 'Castilla', 'Mediterránea', 'Cantábrica'], ['Ingeniería', 'Distribuciones', 'Servicios', 'Energía'], ['S.L.', 'S.A.'],
     ['Pablo', 'Marta', 'Álvaro', 'Carmen', 'Sergio', 'Irene'], ['Fernández', 'López', 'Martínez', 'Sánchez', 'Navarro', 'Vidal']),
    ('Europe', 'Netherlands', ['Rotterdam', 'Eindhoven', 'Utrecht', 'Groningen'],
     ['De Vries', 'Van Dijk', 'Bakker', 'Visser', 'Smit', 'Meijer'], ['Techniek', 'Handel', 'Logistiek', 'Groep'], ['B.V.', 'N.V.'],
     ['Daan', 'Sanne', 'Bram', 'Lotte', 'Thijs', 'Femke'], ['Jansen', 'De Boer', 'Mulder', 'Bos', 'Vos', 'Peters']),
    ('Europe', 'Sweden', ['Göteborg', 'Malmö', 'Uppsala', 'Västerås'],
     ['Nordlund', 'Lindqvist', 'Björk', 'Ekström', 'Sjöberg', 'Holmgren'], ['Teknik', 'Industri', 'Energi', 'Design'], ['AB'],
     ['Erik', 'Astrid', 'Oskar', 'Maja', 'Nils', 'Ingrid'], ['Andersson', 'Johansson', 'Karlsson', 'Nilsson', 'Eriksson', 'Larsson']),
    ('Europe', 'Poland', ['Kraków', 'Wrocław', 'Gdańsk', 'Poznań', 'Łódź'],
     ['Wisła', 'Piast', 'Karpaty', 'Baltyk', 'Mazowsze', 'Jagiellon'], ['Technologie', 'Transport', 'Budownictwo', 'Systemy'], ['Sp. z o.o.', 'S.A.'],
     ['Piotr', 'Zofia', 'Jakub', 'Agnieszka', 'Tomasz', 'Magdalena'], ['Kowalski', 'Wiśniewski', 'Zieliński', 'Szymański', 'Woźniak', 'Dąbrowski']),
    ('Europe', 'Greece', ['Αθήνα', 'Θεσσαλονίκη', 'Πάτρα', 'Ηράκλειο'],
     ['Ελληνική', 'Αιγαίο', 'Ολυμπος', 'Ιόνιο', 'Αρχιμήδης', 'Κρήτη'], ['Τεχνική', 'Εμπορική', 'Ενεργειακή', 'Ναυτιλιακή'], ['Α.Ε.', 'Ε.Π.Ε.'],
     ['Γιώργος', 'Ελένη', 'Νίκος', 'Μαρία', 'Δημήτρης', 'Κατερίνα'], ['Παπαδόπουλος', 'Νικολάου', 'Γεωργίου', 'Αντωνίου', 'Ιωάννου', 'Κωνσταντίνου']),
    ('Europe', 'Ukraine', ['Київ', 'Львів', 'Одеса', 'Харків', 'Дніпро'],
     ['Дніпро', 'Карпати', 'Золота', 'Чорномор', 'Славутич', 'Оріон'], ['Логістик', 'Техно', 'Агро', 'Енерго'], ['ТОВ', 'ПрАТ'],
     ['Андрій', 'Олена', 'Богдан', 'Ірина', 'Тарас', 'Оксана'], ['Шевченко', 'Коваленко', 'Бондаренко', 'Мельник', 'Ткаченко', 'Кравченко']),
    ('Europe', 'Bulgaria', ['София', 'Пловдив', 'Варна', 'Бургас'],
     ['Балкан', 'Рила', 'Витоша', 'Тракия', 'Дунав', 'Родопи'], ['Софтуер', 'Инженеринг', 'Транс', 'Фуудс'], ['ООД', 'АД', 'ЕООД'],
     ['Георги', 'Елица', 'Иван', 'Мария', 'Димитър', 'Силвия'], ['Иванов', 'Петров', 'Георгиев', 'Димитров', 'Стоянов', 'Тодоров']),
    ('Europe', 'Switzerland', ['Zürich', 'Basel', 'Genève', 'Bern', 'Lausanne'],
     ['Helvetia', 'Alpina', 'Rigi', 'Matterhorn', 'Jungfrau', 'Säntis'], ['Präzision', 'Finanz', 'Medtech', 'Technologie'], ['AG', 'SA', 'GmbH'],
     ['Noah', 'Mia', 'Luca', 'Lena', 'Elias', 'Sofia'], ['Meier', 'Keller', 'Huber', 'Frei', 'Brunner', 'Steiner']),
    ('Asia Pacific', 'Japan', ['Tokyo', 'Osaka', 'Nagoya', 'Fukuoka', 'Sapporo'],
     ['Sakura', 'Fuji', 'Hikari', 'Kaze', 'Mizuho', 'Asahi', 'Tsubasa'], ['Precision', 'Electronics', 'Robotics', 'Trading'], ['K.K.', 'Co., Ltd.'],
     ['Haruto', 'Yui', 'Sota', 'Aoi', 'Ren', 'Hina'], ['Tanaka', 'Suzuki', 'Takahashi', 'Watanabe', 'Kobayashi', 'Yamamoto']),
    ('Asia Pacific', 'Australia', ['Sydney', 'Melbourne', 'Brisbane', 'Perth', 'Adelaide'],
     ['Southern Cross', 'Outback', 'Coral', 'Kestrel', 'Banksia', 'Boomerang'], ['Mining', 'Agribusiness', 'Technologies', 'Marine'], ['Pty Ltd', 'Ltd'],
     ['Jack', 'Charlotte', 'Mason', 'Matilda', 'Lachlan', 'Zoe'], ['Wilson', 'Campbell', 'Murray', 'Stewart', 'Anderson', 'Clarke']),
    ('Asia Pacific', 'India', ['Bengaluru', 'Pune', 'Hyderabad', 'Chennai', 'Ahmedabad', 'Mumbai'],
     ['Bharat', 'Ganga', 'Surya', 'Tej', 'Vikram', 'Navrang', 'Kaveri'], ['Infotech', 'Industries', 'Pharma', 'Logistics'], ['Pvt. Ltd.', 'Ltd.'],
     ['Arjun', 'Priya', 'Rohan', 'Ananya', 'Vikram', 'Kavya'], ['Sharma', 'Iyer', 'Reddy', 'Gupta', 'Nair', 'Kapoor']),
    ('Asia Pacific', 'Singapore', ['Singapore'],
     ['Merlion', 'Straits', 'Lion City', 'Orchid', 'Marina', 'Raffles'], ['Capital', 'Maritime', 'Digital', 'Holdings'], ['Pte. Ltd.'],
     ['Wei', 'Mei', 'Jun', 'Hui', 'Kai', 'Li'], ['Tan', 'Lim', 'Ng', 'Chong', 'Ong', 'Teo']),
    ('Asia Pacific', 'South Korea', ['Seoul', 'Busan', 'Incheon', 'Daejeon'],
     ['Hanbit', 'Daeyang', 'Miraero', 'Sinhan', 'Jeil', 'Haneul'], ['Semiconductor', 'Motors', 'Display', 'Logistics'], ['Co., Ltd.'],
     ['Min-jun', 'Seo-yeon', 'Ji-ho', 'Ha-eun', 'Do-yun', 'Soo-ah'], ['Kim', 'Lee', 'Park', 'Choi', 'Jung', 'Kang']),
    ('Asia Pacific', 'New Zealand', ['Auckland', 'Wellington', 'Christchurch', 'Dunedin'],
     ['Kiwi', 'Tasman', 'Kauri', 'Fernleaf', 'Aotea', 'Pohutukawa'], ['Dairy', 'Timber', 'Software', 'Tourism'], ['Ltd'],
     ['Hemi', 'Aroha', 'Tane', 'Maia', 'Liam', 'Isla'], ['Williams', 'Ngata', 'Parata', 'Walker', 'Thompson', 'Brown']),
    ('Latin America', 'Brazil', ['São Paulo', 'Rio de Janeiro', 'Curitiba', 'Belo Horizonte', 'Porto Alegre', 'Recife'],
     ['Aurora', 'Tupã', 'Bandeirante', 'Cruzeiro', 'Amazônia', 'Ipanema', 'Brasa'], ['Comércio', 'Engenharia', 'Alimentos', 'Logística'], ['Ltda.', 'S.A.'],
     ['Gabriel', 'Beatriz', 'Rafael', 'Mariana', 'Thiago', 'Larissa'], ['Silva', 'Santos', 'Oliveira', 'Souza', 'Costa', 'Pereira']),
    ('Latin America', 'Argentina', ['Buenos Aires', 'Córdoba', 'Rosario', 'Mendoza'],
     ['Pampa', 'Patagonia', 'Andina', 'Río de la Plata', 'Gaucho', 'Cóndor'], ['Agro', 'Industrias', 'Servicios', 'Tecnología'], ['S.A.', 'S.R.L.'],
     ['Mateo', 'Camila', 'Santiago', 'Julieta', 'Facundo', 'Agustina'], ['González', 'Rodríguez', 'Fernández', 'Gómez', 'Díaz', 'Acosta']),
    ('Latin America', 'Chile', ['Santiago', 'Valparaíso', 'Concepción', 'Antofagasta'],
     ['Atacama', 'Araucanía', 'Maipo', 'Cordillera', 'Pacífico', 'Los Andes'], ['Minería', 'Vinos', 'Energía', 'Ingeniería'], ['SpA', 'Ltda.'],
     ['Benjamín', 'Antonia', 'Vicente', 'Fernanda', 'Joaquín', 'Constanza'], ['Soto', 'Contreras', 'Silva', 'Rojas', 'Muñoz', 'Araya']),
    ('Latin America', 'Colombia', ['Bogotá', 'Medellín', 'Cali', 'Barranquilla'],
     ['Esmeralda', 'Cafetera', 'Caribe', 'Andes', 'Orinoco', 'Tairona'], ['Comercializadora', 'Tecnología', 'Constructora', 'Logística'], ['S.A.S.', 'Ltda.'],
     ['Santiago', 'Valeria', 'Sebastián', 'Isabella', 'Nicolás', 'Daniela'], ['Ramírez', 'Torres', 'Vargas', 'Rincón', 'Mejía', 'Cardona']),
    ('Middle East & Africa', 'United Arab Emirates', ['Dubai', 'Abu Dhabi', 'Sharjah'],
     ['Falcon', 'Oasis', 'Gulf', 'Pearl', 'Desert Rose', 'Burj'], ['Trading', 'Holdings', 'Logistics', 'Technologies'], ['LLC', 'FZE'],
     ['Omar', 'Layla', 'Khalid', 'Noor', 'Yusuf', 'Mariam'], ['Al Mansoori', 'Al Hashimi', 'Khan', 'Haddad', 'Farouk', 'Saleh']),
    ('Middle East & Africa', 'Saudi Arabia', ['Riyadh', 'Jeddah', 'Dammam'],
     ['Najd', 'Red Sea', 'Tuwaiq', 'Arabian', 'Al Noor', 'Rawabi'], ['Contracting', 'Petrochemicals', 'Trading', 'Technology'], ['Co.', 'LLC'],
     ['Faisal', 'Sara', 'Abdullah', 'Huda', 'Fahad', 'Reem'], ['Al Qahtani', 'Al Harbi', 'Al Otaibi', 'Bakr', 'Zahrani', 'Mutairi']),
    ('Middle East & Africa', 'South Africa', ['Johannesburg', 'Cape Town', 'Durban', 'Pretoria'],
     ['Springbok', 'Karoo', 'Table Mountain', 'Protea', 'Limpopo', 'Savanna'], ['Mining', 'Logistics', 'Agri', 'Software'], ['(Pty) Ltd', 'Ltd'],
     ['Thabo', 'Naledi', 'Pieter', 'Lindiwe', 'Sipho', 'Anika'], ['Naidoo', 'Van der Merwe', 'Dlamini', 'Botha', 'Khumalo', 'Pillay']),
    ('Middle East & Africa', 'Nigeria', ['Lagos', 'Abuja', 'Port Harcourt', 'Kano'],
     ['Niger', 'Eko', 'Zuma', 'Delta', 'Harmattan', 'Ife'], ['Energy', 'Telecoms', 'Agro', 'Logistics'], ['Ltd', 'Nigeria Ltd'],
     ['Chinedu', 'Amara', 'Tunde', 'Ngozi', 'Emeka', 'Folake'], ['Okafor', 'Adeyemi', 'Balogun', 'Eze', 'Ibrahim', 'Okonkwo']),
    ('Middle East & Africa', 'Kenya', ['Nairobi', 'Mombasa', 'Kisumu'],
     ['Safari', 'Rift Valley', 'Kilimanjaro', 'Mara', 'Jambo', 'Savannah'], ['Agri', 'Tech', 'Logistics', 'Tours'], ['Ltd'],
     ['Wanjiru', 'Kamau', 'Achieng', 'Otieno', 'Njeri', 'Mwangi'], ['Kariuki', 'Odhiambo', 'Mutua', 'Wafula', 'Kiprop', 'Chebet']),
]
SEGMENTS = [('Enterprise', 0.17, 7.0), ('Mid-Market', 0.27, 3.0), ('Small Business', 0.24, 1.2), ('Startup', 0.12, 1.0),
            ('Education', 0.08, 1.4), ('Government', 0.07, 2.2), ('Non-profit', 0.05, 0.6)]
INDUSTRIES = ['Manufacturing', 'Retail', 'Healthcare', 'Finance', 'Technology', 'Energy', 'Logistics', 'Hospitality', 'Construction', 'Agriculture', 'Media', 'Telecommunications']
NOUN_BY_INDUSTRY = None

# ---- products ----------------------------------------------------------------------------------------------------------
PRODUCTS = [  # category, name, price, cost ratio
    ('Compute', 'Atlas R200 Rack Server', 6400, .62), ('Compute', 'Atlas R400 Rack Server', 11800, .63), ('Compute', 'Atlas T100 Tower Server', 3200, .60),
    ('Compute', 'Nimbus 14 Business Laptop', 1450, .70), ('Compute', 'Nimbus 16 Workstation', 2890, .68), ('Compute', 'Pulse Mini Desktop', 780, .72),
    ('Networking', 'Vector 24-port Switch', 940, .55), ('Networking', 'Vector 48-port PoE Switch', 2650, .56), ('Networking', 'Orbit Wi-Fi 6 Access Point', 310, .52),
    ('Networking', 'Bastion Firewall FW-500', 4900, .50), ('Networking', 'Bastion Firewall FW-2000', 14800, .48), ('Networking', 'Relay SD-WAN Edge', 2100, .54),
    ('Storage', 'Vault 8-bay NAS', 2300, .64), ('Storage', 'Vault 24-bay Array', 18900, .61), ('Storage', 'Ledger Backup Appliance', 7600, .58),
    ('Storage', 'Quartz NVMe 3.8 TB', 640, .74), ('Storage', 'Archive Tape Library', 9800, .66),
    ('Software', 'Compass Analytics (per seat)', 220, .12), ('Software', 'Compass Analytics Enterprise (per seat)', 480, .12), ('Software', 'Shield Endpoint Security (per seat)', 58, .15),
    ('Software', 'Forge CI/CD Suite (per seat)', 140, .14), ('Software', 'Lantern Monitoring (per node)', 95, .13), ('Software', 'Atlas OS Server License', 760, .10),
    ('Services', 'Premium Support, 1 year', 3600, .35), ('Services', 'Premium Support, 3 years', 9400, .33), ('Services', 'Installation & Setup (day)', 1450, .55),
    ('Services', 'Security Audit', 8900, .45), ('Services', 'Cloud Migration Project', 26500, .50), ('Services', 'Training Workshop (day)', 2100, .40),
    ('Accessories', 'Ergo Keyboard & Mouse Set', 89, .60), ('Accessories', 'UltraWide 34" Monitor', 620, .72), ('Accessories', 'Docking Station Pro', 240, .66),
    ('Accessories', 'Rack Mount Kit', 130, .50), ('Accessories', 'Cable Pack (50 pcs)', 75, .45), ('Accessories', 'UPS 1500VA', 520, .68),
]
CATEGORY_QTY = {'Compute': (1, 8), 'Networking': (1, 10), 'Storage': (1, 6), 'Software': (5, 120), 'Services': (1, 4), 'Accessories': (2, 40)}
CATEGORY_PULL = {'Compute': 1.0, 'Networking': 0.8, 'Storage': 0.55, 'Software': 0.9, 'Services': 0.6, 'Accessories': 0.7}

EMPLOYEES = [  # name, title, region, hired, annual quota
    ('Elena Vasquez', 'Regional Director', 'North America', '2016-03-14', 2_400_000), ('Marcus Whitfield', 'Senior Account Executive', 'North America', '2018-06-01', 1_500_000),
    ('Priya Raman', 'Account Executive', 'North America', '2021-09-20', 1_100_000), ('Henrik Lundqvist', 'Regional Director', 'Europe', '2015-11-02', 2_600_000),
    ('Sofia Marchetti', 'Senior Account Executive', 'Europe', '2017-04-18', 1_700_000), ('Jakub Nowak', 'Account Executive', 'Europe', '2020-02-10', 1_200_000),
    ('Aiko Nakamura', 'Regional Director', 'Asia Pacific', '2017-08-07', 2_000_000), ('Daniel Okoye', 'Account Executive', 'Asia Pacific', '2022-01-17', 900_000),
    ('Camila Duarte', 'Senior Account Executive', 'Latin America', '2019-05-27', 1_300_000), ('Omar Haddad', 'Account Executive', 'Middle East & Africa', '2019-10-14', 1_000_000),
]

def q(v):
    if v is None:
        return 'NULL'
    if isinstance(v, (int, float)):
        return repr(round(v, 2)) if isinstance(v, float) else str(v)
    return "'" + str(v).replace("'", "''") + "'"

def rows(table, cols, data, batch=40):
    out = []
    for i in range(0, len(data), batch):
        chunk = data[i:i + batch]
        out.append(f"INSERT INTO {table}({', '.join(cols)}) VALUES\n" + ',\n'.join('  (' + ', '.join(q(v) for v in r) + ')' for r in chunk) + ';')
    return '\n'.join(out)

def weighted(items, weights):
    return rnd.choices(items, weights=weights, k=1)[0]

# ---- customers ---------------------------------------------------------------------------------------------------------
employees = [(i + 1, *e) for i, e in enumerate(EMPLOYEES)]
reps_by_region = {}
for e in employees:
    reps_by_region.setdefault(e[3], []).append(e[0])

customers = []
used = set()
REGION_PULL = {'North America': 2.3, 'Europe': 1.1, 'Asia Pacific': 1.2, 'Latin America': 1.1, 'Middle East & Africa': 1.0}
weights = [REGION_PULL[p[0]] for p in PLACES]
while len(customers) < 150:
    region, country, cities, words, nouns, suffixes, firsts, lasts = weighted(PLACES, weights)
    seg = weighted([s[0] for s in SEGMENTS], [s[1] for s in SEGMENTS])
    industry = rnd.choice(INDUSTRIES)
    form = rnd.randrange(3)
    word, noun, suffix = rnd.choice(words), rnd.choice(nouns), rnd.choice(suffixes)
    if seg in ('Education', 'Non-profit', 'Government'):
        kind = {'Education': ['University of', 'Institute of Technology,', 'College of'], 'Non-profit': ['Foundation', 'Alliance for', 'Trust'], 'Government': ['Municipality of', 'Ministry Office,', 'Regional Authority of']}[seg]
        city = rnd.choice(cities)
        name = f"{rnd.choice(kind)} {city}" if country not in ('Greece', 'Ukraine', 'Bulgaria') else f"{city} {noun}"
    elif form == 0:
        name = f"{word} {noun} {suffix}"
    elif form == 1:
        name = f"{rnd.choice(lasts)} & {rnd.choice(lasts)} {noun} {suffix}" if rnd.random() < .5 else f"{word} {rnd.choice(lasts)} {suffix}"
    else:
        name = f"{word} {rnd.choice(nouns)} {suffix}"
    name = ' '.join(name.split())
    if name in used:
        continue
    used.add(name)
    city = rnd.choice(cities)
    since = datetime.date(2014, 1, 1) + datetime.timedelta(days=rnd.randrange(0, 365 * 11))
    credit = {'Enterprise': 250_000, 'Mid-Market': 80_000, 'Small Business': 20_000, 'Startup': 15_000, 'Education': 60_000, 'Government': 120_000, 'Non-profit': 25_000}[seg] * rnd.choice([0.5, 1, 1, 1.5, 2])
    contact = f"{rnd.choice(firsts)} {rnd.choice(lasts)}"
    rep = rnd.choice(reps_by_region[region])
    activity = rnd.lognormvariate(0, 0.9)
    customers.append([len(customers) + 1, name, country, region, city, seg, industry, since.isoformat(), rep, round(credit), contact, activity])

# ---- orders ------------------------------------------------------------------------------------------------------------
products = [(i + 1, f"{c[:3].upper()}-{i + 1:03d}", n, c, float(p), round(p * r, 2)) for i, (c, n, p, r) in enumerate(PRODUCTS)]
seg_weight = {s[0]: s[2] for s in SEGMENTS}
cust_weights = [seg_weight[c[5]] * c[11] for c in customers]

orders, items = [], []
month = datetime.date(2025, 1, 1)
index = 0
while month <= datetime.date(2026, 10, 1):
    seasonal = {3: 3, 6: 2, 9: 3, 11: 7, 12: 5, 1: -2, 8: -2}.get(month.month, 0)
    count = max(6, round(11 + 0.5 * index + seasonal + rnd.randint(-2, 2)))
    for _ in range(count):
        c = weighted(customers, cust_weights)
        last = (datetime.date(month.year + (month.month == 12), month.month % 12 + 1, 1) - datetime.timedelta(days=1)).day
        day = datetime.date(month.year, month.month, rnd.randint(1, last))
        if day > TODAY:
            day = TODAY - datetime.timedelta(days=rnd.randint(0, 6))
        age = (TODAY - day).days
        r = rnd.random()
        if age > 45:
            status = 'Delivered' if r < .88 else 'Returned' if r < .93 else 'Cancelled'
        elif age > 10:
            status = 'Delivered' if r < .55 else 'Shipped' if r < .95 else 'Cancelled'
        else:
            status = 'Pending' if r < .6 else 'Shipped'
        ship = None if status in ('Pending', 'Cancelled') else (day + datetime.timedelta(days=rnd.randint(1, 6))).isoformat()
        channel = weighted(['Web', 'Direct', 'Partner', 'Phone'], [.3, .35 if c[5] in ('Enterprise', 'Government') else .15, .22, .13])
        discount = weighted([0, .02, .05, .08, .1, .15], [.3, .1, .25, .15, .12, .08] if c[5] != 'Enterprise' else [.05, .05, .2, .2, .3, .2])
        oid = len(orders) + 1
        orders.append((oid, c[0], c[8], day.isoformat(), ship, status, channel, discount))
        lines = rnd.randint(2, 6) if c[5] in ('Enterprise', 'Government') else rnd.randint(1, 4)
        chosen = set()
        for _ in range(lines):
            p = weighted(products, [CATEGORY_PULL[x[3]] for x in products])
            if p[0] in chosen:
                continue
            chosen.add(p[0])
            lo, hi = CATEGORY_QTY[p[3]]
            scale = 3 if c[5] == 'Enterprise' and p[3] in ('Software', 'Accessories', 'Compute') else 1
            qty = max(1, round(rnd.randint(lo, hi) * (rnd.random() ** 1.6) * scale)) if hi > 1 else 1
            price = round(p[4] * rnd.choice([1, 1, 1, 0.97, 1.03]), 2)
            items.append((len(items) + 1, oid, p[0], qty, price))
    month = datetime.date(month.year + (month.month == 12), month.month % 12 + 1, 1)
    index += 1

# Quotas follow what each rep really sold in 2025 (from 80% to 130% of it), so the leaderboard has winners and laggards.
price = {p[0]: p for p in products}
sold = {}
by_order = {o[0]: o for o in orders}
for it in items:
    o = by_order[it[1]]
    if o[3].startswith('2025') and o[5] != 'Cancelled':
        sold[o[2]] = sold.get(o[2], 0) + it[3] * it[4] * (1 - o[7])
employees = [(e[0], e[1], e[2], e[3], e[4], float(round(sold.get(e[0], 300_000) * rnd.uniform(0.8, 1.3) / 10_000) * 10_000)) for e in employees]

# ---- output -----------------------------------------------------------------------------------------------------------
print('-- Generated by generate.py: a fictional B2B sales database. Every company, person and figure is made up.')
print('BEGIN;')
print('''CREATE TABLE employees(
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, title TEXT, region TEXT NOT NULL, hired TEXT, annual_quota REAL);
CREATE TABLE customers(
  id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, country TEXT NOT NULL, region TEXT NOT NULL, city TEXT, segment TEXT NOT NULL,
  industry TEXT, customer_since TEXT, rep_id INTEGER REFERENCES employees(id), credit_limit REAL, contact TEXT);
CREATE TABLE products(
  id INTEGER PRIMARY KEY, sku TEXT NOT NULL UNIQUE, name TEXT NOT NULL, category TEXT NOT NULL, unit_price REAL NOT NULL, unit_cost REAL NOT NULL);
CREATE TABLE orders(
  id INTEGER PRIMARY KEY, customer_id INTEGER NOT NULL REFERENCES customers(id), rep_id INTEGER REFERENCES employees(id),
  order_date TEXT NOT NULL, ship_date TEXT, status TEXT NOT NULL, channel TEXT NOT NULL, discount REAL NOT NULL DEFAULT 0);
CREATE TABLE order_items(
  id INTEGER PRIMARY KEY, order_id INTEGER NOT NULL REFERENCES orders(id), product_id INTEGER NOT NULL REFERENCES products(id),
  quantity INTEGER NOT NULL, unit_price REAL NOT NULL);
CREATE INDEX orders_by_customer ON orders(customer_id);
CREATE INDEX orders_by_date ON orders(order_date);
CREATE INDEX items_by_order ON order_items(order_id);

-- One row per order line that counts as sales (cancelled orders do not), with everything a report groups by.
CREATE VIEW sales AS
SELECT i.id AS line_id, o.id AS order_id, o.order_date,
       CAST(substr(o.order_date, 1, 4) AS INTEGER) AS year,
       substr(o.order_date, 1, 7) AS month,
       'Q' || ((CAST(substr(o.order_date, 6, 2) AS INTEGER) + 2) / 3) AS quarter,
       c.id AS customer_id, c.name AS customer, c.country, c.region, c.segment, c.industry,
       e.name AS rep, p.id AS product_id, p.category, p.name AS product,
       i.quantity, i.unit_price, o.discount,
       round(i.quantity * i.unit_price * (1 - o.discount), 2) AS revenue,
       round(i.quantity * p.unit_cost, 2) AS cost,
       o.status, o.channel
FROM order_items i
JOIN orders o ON o.id = i.order_id
JOIN customers c ON c.id = o.customer_id
JOIN products p ON p.id = i.product_id
LEFT JOIN employees e ON e.id = o.rep_id
WHERE o.status <> 'Cancelled';
''')
print(rows('employees', ['id', 'name', 'title', 'region', 'hired', 'annual_quota'], employees))
print(rows('customers', ['id', 'name', 'country', 'region', 'city', 'segment', 'industry', 'customer_since', 'rep_id', 'credit_limit', 'contact'], [c[:11] for c in customers]))
print(rows('products', ['id', 'sku', 'name', 'category', 'unit_price', 'unit_cost'], products))
print(rows('orders', ['id', 'customer_id', 'rep_id', 'order_date', 'ship_date', 'status', 'channel', 'discount'], orders))
print(rows('order_items', ['id', 'order_id', 'product_id', 'quantity', 'unit_price'], items, 80))
print('COMMIT;')
import sys
print(f'-- {len(employees)} employees, {len(customers)} customers, {len(products)} products, {len(orders)} orders, {len(items)} order lines', file=sys.stderr)
