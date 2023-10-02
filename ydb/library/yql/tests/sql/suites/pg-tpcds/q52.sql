--!syntax_pg
--TPC-DS Q52

-- start query 1 in stream 0 using template ../query_templates/query52.tpl
select  dt.d_year
 	,item.i_brand_id brand_id
 	,item.i_brand brand
 	,sum(ss_ext_sales_price) ext_price
 from plato.date_dim dt
     ,plato.store_sales
     ,plato.item
 where dt.d_date_sk = store_sales.ss_sold_date_sk
    and store_sales.ss_item_sk = item.i_item_sk
    and item.i_manager_id = 1
    and dt.d_moy=12
    and dt.d_year=1998
 group by dt.d_year
 	,item.i_brand
 	,item.i_brand_id
 order by dt.d_year
 	,ext_price desc
 	,brand_id
limit 100 ;

-- end query 1 in stream 0 using template ../query_templates/query52.tpl