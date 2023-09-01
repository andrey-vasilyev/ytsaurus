# Введение

В этой статье дается обоснование выбранной модели предоставления ClickHouse внутри экосистемы {{product-name}}. В частности, почему в CHYT есть какие-то *клики*, тогда как в обычном ClickHouse ничего похожего нет.

## Что такое клика? { #what-is }

*Клика* представляет из себя полноценный кластер ClickHouse, в который встроена возможность читать данные из таблиц, находящихся в {{product-name}}, посредством нативного внутреннего протокола {{product-name}}.
С точки зрения [планировщика {{product-name}}](../../../../user-guide/data-processing/scheduler/scheduler-and-pools.md) клика — это Vanilla-операция, содержащая в типичном случае до десятков джобов, каждый из которых потребляет по умолчанию до 16 ядер CPU. Название *клика* было выбрано в качестве игры слов по созвучию с названием ClickHouse, но при этом правильное написание по-английски — *Clique*.

Фундаментальное свойство CHYT — "администрирование" клики осуществляется *пользователями*, а не администраторами {{product-name}} (подробнее см. [Администрирование приватной клики](../../../../user-guide/data-processing/chyt/cliques/administration.md)). В частности, любой пользователь {{product-name}} может в любой момент запустить свою собственную клику (или даже несколько клик).


## Надежная изоляция { #isolation }

Благодаря такому представлению решается задача *изоляции* нагрузки от различных пользователей. На OLAP-базы данных типа ClickHouse может приходиться нагрузка принципиально разного устройства. Можно отметить продакшн-процессы типа ETL, создающие постоянный фон нагрузки из-под роботных пользователей. Помимо этого бывают разовые ad-hoc запросы от пользователей-людей через инструменты типа YQL, а также разовые или регулярные выгрузки датасетов во внешние BI-системы, такие так [DataLens](https://cloud.yandex.ru/docs/datalens/) или [Tableau](https://www.tableau.com/) для дальнейшей визуализации.

Если на базу данных приходится много одновременно заданных "тяжелых" запросов, это может перегрузить кластер множеством различных способов:
- На вычислительных узлах могут закончиться ресурсы CPU, вследствие чего все запросы (включая быстрые в нормальных обстоятельствах) начнут получать меньше CPU time и произвольным образом деградируют.
- Как следствие предыдущего может деградировать подсистема IO или сетевой стек (например, TCP или DNS-подсистема), которым тоже нужен CPU time для функционирования.
- Вычислительные узлы могут упереться в сетевую полосу и стать [IO-bound](https://en.wikipedia.org/wiki/I/O_bound).
- Наконец, обработка существенных объемов данных может потреблять очень много оперативной памяти — как непосредственно для проведения расчётов, требующих поддержания больших промежуточных состояний (таких, как тяжелые `GROUP BY`, сортировки или `COUNT DISTINCT`), так и элементарно для чтения данных по сети под буфера.

Несмотря на то, что в ClickHouse встроены отдельные защитные механизмы для решения описанных выше проблем, в реалиях {{product-name}} они спасают далеко не всегда. Ключевое свойство {{product-name}} как источника данных для ClickHouse — любая хоть сколько-то большая таблица является "очень сильно распределенной". Грубо говоря, можно считать, что каждый следующий ГБ данных в таблице будет жить на новом хосте. Помимо этого  [статические таблицы {{product-name}}](../../../../user-guide/storage/static-tables.md), а особенно те, которые хранят аналитические данные, зачастую могут быть сжаты тяжелым кодеком компрессии (смотрите [Сжатие](../../../../user-guide/storage/compression.md)).

Эти обстоятельства приводят к тому, что обработка даже "сравнительно легкой" таблицы в десяток ГБ с извлечением только нескольких узких колонок суммарным весом в сотни МБ оказывается сильно завязана на сетевой стек и на подсистему декомпрессии данных, а также довольно сильно требовательна к RAM для буферов чтения по сети с каждой машины.

Не желая решать сложную задачу менеджмента ресурсов CPU, RAM, полосы сетевого и дискового IO внутри ClickHouse, было принято решение разделять пользователей физически по разным процессам, что как раз достигается заведением необходимого количества независимых друг от друга клик, использующихся небольшими группами пользователей. Ожидается, что клики будут достаточно мелкогранулярными, чтобы не обслуживать разные виды нагрузки либо большое количество пользователей одновременно.

Единственное исключение из данного правила — [публичные клики](../../../../user-guide/data-processing/chyt/try-chyt.md#public), которые намеренно открыты для всех, но предоставляются по принципу best effort — они могут быть в произвольный момент недоступны либо перегружены, хотя за их работоспособностью ведется наблюдение. Есть ограничения на сложность исполняемых запросов, местами довольно жесткие. Следует воспринимать публичные клики как демо-версию CHYT.

## CHYT as a service: переиспользование ресурсов в {{product-name}} и эксплуатация силами пользователей { #as-a-service }

Подобная система разделения ресурсов по разным кликам в рамках одного большого кластера {{product-name}} напоминает "облачный" хостинг ClickHouse, похожий на [Managed ClickHouse](https://cloud.yandex.ru/services/managed-clickhouse) в облачном провайдере. Единственное отличие — пользователи платят не рублями во внешнем облаке, а вычислительной квотой в {{product-name}}. Для многих аналитических и разработческих команд это приводит к возможности гибко распоряжаться уже существующими вычислительными ресурсами в {{product-name}}. Часть из них можно использовать в "классических" вычислительных пайплайнах на минуты/часы/дни с Map-Reduce, например, посредством YQL over {{product-name}}. А на другую небольшую часть — поднять клику подходящего размера для быстрых ad-hoc вычислений с задержками в секунды.

Помимо этого, как и всегда с облачным технологиями, предоставление чего-либо по модели "as a service" позволяет четко обозначить границу ответственности за эксплуатацию тех или иных технических решений.

Наибольшие успехи в эксплуатации любой базы данных достигаются при одновременном выполнении двух условий:

- Пользователь хорошо понимает устройство базы данных и в состоянии оценить узкие места будущего вычислительного пайплайна с ее использованием.
- Пользователь хорошо знает специфику хранящихся данных, а также необходимых вычислений над ними, а также знает, откуда эти данные возникают в базе данных, и отвечает за их конкретное представление и форму хранения.

Ожидается, что с помощью документации пользователи смогут решать большую часть эксплуатационных проблем, в том числе проводить диагностику медленных запросов и грамотно оценивать потребность в ресурсах.