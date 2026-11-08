#!/usr/bin/env python3
"""Genera conocimiento expositivo y episodios de razonamiento deliberativo + dialogo multi-turno.

Escribe en cortex-lm/data/ (ignorado por git):
  - data/conocimiento_es.txt : prosa expositiva rica sobre ciencia, fisica, logica,
                               neurobiologia, lengua, historia y literatura hispanica.
  - data/alineacion_es.txt   : episodios para la via dorsal ejecutiva en tres formatos:
      1) Deliberativo:  'Pregunta\\n? [[paso 1 -> paso 2 -> conclusion]]\\n> Respuesta'
      2) Directo:       'Pregunta\\n> Respuesta'
      3) Multi-turno:   'Turno1\\n---\\nPregunta_seguimiento\\n? [[...]]\\n> Respuesta'
         (entrenado con Engine::carry_context(0.48f) exacto entre turnos).
"""

import os
import re

EXPOSICION = """
El idioma español es una lengua romance nacida de la evolución del latín hablado en la península ibérica. Hoy lo hablan cientos de millones de personas en América, Europa y otras regiones del mundo. Su ortografía representa con gran regularidad los sonidos del habla, distinguida por el uso de tildes gráficas que marcan la sílaba tónica según reglas claras y precisas.

El agua es una sustancia esencial compuesta por hidrógeno y oxígeno. A nivel del mar se congela a cero grados Celsius formando hielo sólido y hierve a cien grados Celsius transformándose en vapor. A diferencia de casi todos los líquidos, el hielo es menos denso que el agua líquida porque sus moléculas forman una red cristalina abierta; por esa razón física el hielo flota sobre ríos y mares, aislando la vida acuática del frío extremo.

En física, la luz viaja en el aire a casi trescientos mil kilómetros por segundo, mientras que el sonido avanza a unos trescientos cuarenta metros por segundo. Durante una tormenta eléctrica el relámpago y el trueno nacen al mismo tiempo en la descarga del rayo, pero nuestros ojos perciben el destello casi al instante y nuestros oídos reciben el estruendo varios segundos después.

El sistema solar está presidido por el Sol, una estrella con luz propia generada por fusión nuclear en su interior, mientras que los planetas son cuerpos que no emiten luz propia sino que reflejan la del Sol. Alrededor del Sol giran ocho planetas principales: Mercurio, Venus, la Tierra, Marte, Júpiter, Saturno, Urano y Neptuno. La Tierra gira sobre su eje cada veinticuatro horas originando el día y la noche, y recorre su órbita anual en trescientos sesenta y cinco días; la inclinación de su eje produce las estaciones del año.

Las neuronas biológicas de la corteza cerebral se organizan en regiones jerárquicas comunicadas mediante espigas discretas. Las dendritas basales detectan coincidencias sensoriales ascendentes con retardos axonales precisos, mientras que el penacho dendrítico apical recibe predicciones contextuales descendentes desde regiones prefrontales superiores. Las trazas sinápticas lentas tipo NMDA sostienen los conceptos activos durante decenas de símbolos y la subregión hipocampal CA3 permite memorizar episodios nuevos en un solo disparo mediante plasticidad Hebbiana dispersa.

El razonamiento deductivo consiste en encadenar premisas verdaderas para llegar con certeza a una conclusión necesaria. Si sabemos que todos los mamíferos respiran aire con pulmones y que la ballena es un mamífero marino, se deduce necesariamente que la ballena sale a la superficie para respirar aire. Del mismo modo, la relación mayor que es transitiva: si Ana tiene más edad que Luis y Luis tiene más edad que Pedro, entonces Ana es necesariamente mayor que Pedro.

En la naturaleza, las plantas realizan la fotosíntesis utilizando la energía de la luz solar para transformar dióxido de carbono y agua en azúcares nutritivos, liberando oxígeno a la atmósfera. Sin las plantas disminuiría el oxígeno respirable y colapsaría la cadena alimentaria terrestre.

Las matemáticas permiten razonar con exactitud sobre cantidades y proporciones. Un kilogramo de hierro y un kilogramo de algodón tienen exactamente la misma masa de un kilogramo, aunque el algodón ocupe mucho más volumen por tener menor densidad. Multiplicar tres cajas por cinco libros en cada caja reúne quince libros en total.

En literatura hispánica destacan obras universales como Don Quijote de la Mancha, escrita por Miguel de Cervantes en dos partes publicadas en 1605 y 1615, donde el caballero Alonso Quijano recorre la Mancha junto a su fiel escudero Sancho Panza; las novelas realistas de Benito Pérez Galdós en el siglo diecinueve como Marianela, Tormento y Nazarín; la novela La Regenta de Leopoldo Alas Clarín; y las Rimas y Leyendas de Gustavo Adolfo Bécquer.

Dormir bien cada noche permite al cerebro consolidar la memoria mediante la reactivación sináptica entre el hipocampo y la corteza, fortaleciendo lo aprendido durante el día sin borrar los conocimientos previos.

En geografía de América del Sur destacan la cordillera de los Andes, que recorre el continente de norte a sur bordeando el océano Pacífico, y la cuenca del río Amazonas, la mayor selva tropical y reserva fluvial del planeta. En el norte de Sudamérica se encuentra Venezuela, cuya capital es Caracas, situada en un valle al pie del cerro El Ávila junto al mar Caribe, y donde también se elevan los Andes merideños y el Salto Ángel, la caída de agua más alta del mundo.

En biología humana, el corazón funciona como una bomba muscular rítmica que impulsa la sangre por arterias, capilares y venas para llevar oxígeno y nutrientes a cada órgano. En el núcleo de las células se encuentra el ácido desoxirribonucleico o ADN, una doble hélice molecular que almacena las instrucciones hereditarias de todos los seres vivos.

La fuerza de gravedad es la atracción universal entre todos los cuerpos con masa. En la Tierra nos mantiene unidos al suelo y hace caer los objetos hacia el centro del planeta, mientras que en el espacio mantiene a la Luna en órbita alrededor de la Tierra y a los planetas girando alrededor del Sol.

La invención de la imprenta de tipos móviles por Johannes Gutenberg en el siglo quince transformó la historia humana al permitir reproducir libros con rapidez y precisión, impulsando la alfabetización, la difusión de las ciencias y el Renacimiento europeo.

En informática de sistemas, el lenguaje C++ permite escribir motores de alto rendimiento con control directo sobre la memoria contigua, los punteros y la vectorización del procesador, logrando máxima velocidad sin recolección de basura ni sobrecarga innecesaria.
""".strip()

# (lista_preguntas, pensamiento_interno, respuesta_final)
GRUPOS_RAZONAMIENTO = [
    # 1. Identidad, arquitectura 4 regiones + apical + CA3 + pensamiento interno
    (
        ["Hola", "Hola, ¿cómo estás?", "Buenos días", "Buenas tardes, ¿cómo te encuentras?"],
        "saludo cordial en espanol -> responder con claridad y disposicion",
        "Hola, estoy muy bien y listo para razonar y conversar contigo en español."
    ),
    (
        ["¿Qué eres?", "¿Quién eres?", "¿Qué eres y cómo funcionas?", "Háblame de tu arquitectura."],
        "identidad -> CORTEX-LM -> 4 regiones LIF + dendritas apicales + NMDA + CA3 en C++ sin backprop",
        "Soy CORTEX-LM, un cerebro de lenguaje en español con cuatro regiones corticales de neuronas LIF puras, retroalimentación apical, memoria NMDA e hipocampo CA3 en C++, sin retropropagación."
    ),
    (
        ["¿Cómo piensas antes de responder?", "¿Puedes razonar paso a paso?", "¿Cómo funciona tu razonamiento interno?"],
        "pregunta -> deliberacion interna con espigas LIF -> activar asambleas intermedias L2/L3 -> emitir respuesta",
        "Antes de responder genero una cadena interna de conceptos que realimenta mis capas corticales LIF y enciende las asambleas intermedias necesarias para construir la conclusión."
    ),
    (
        ["¿Cómo aprenden tus neuronas?", "¿Usas backpropagation?", "¿Cómo entrenas sin retropropagación?"],
        "aprendizaje biologico -> cero backpropagation -> coincidencia dendritica + STDP + lectura dispersa local",
        "No uso retropropagación; aprendo exclusivamente con mecanismos locales: coincidencia dendrítica basal y apical, inhibición por columnas, STDP y consolidación sináptica."
    ),
    (
        ["¿Qué es una neurona LIF?", "Explícame cómo funciona una neurona pulsante LIF."],
        "neurona LIF -> integra corrientes con fuga -> cruza umbral -> emite espiga y periodo refractario",
        "Una neurona LIF integra pulsos sinápticos con una fuga exponencial de membrana; al superar su umbral emite una espiga discreta y entra en período refractario."
    ),
    (
        ["¿Por qué eres escalable?", "¿Por qué usas grafos dispersos CSR?", "¿Por qué tu memoria no crece con textos largos?"],
        "escalabilidad -> grafos CSR dispersos + arreglos planos SoA + decaimiento por eventos -> RAM fija O(1)",
        "Uso grafos dispersos CSR, arreglos planos en C++ y actualización dirigida por eventos, escalando a cientos de miles de neuronas con consumo de memoria constante."
    ),
    (
        ["¿Cómo recuerdas datos nuevos al instante?", "¿Qué hace tu región hipocampal CA3?", "¿Qué es el aprendizaje continuo?"],
        "memoria -> hipocampo CA3 Hebbiano en 1 disparo + consolidacion elastica cortical -> recordar sin olvidar",
        "Combino una subregión hipocampal CA3 que forma engramas Hebbianos en un solo disparo con consolidación sináptica cortical para aprender sin olvidar lo anterior."
    ),

    # 2. Razonamiento causal fisico y natural (causa -> mecanismo -> efecto)
    (
        ["¿Por qué flota el hielo en el agua?",
         "¿Por qué el hielo no se hunde en un vaso de agua?",
         "Explica por qué flota el hielo sobre el agua.",
         "¿Por qué razón física flota el hielo cuando el agua se congela?"],
        "agua congelada -> red cristalina abierta -> menor densidad que agua liquida -> flota",
        "El hielo flota porque al congelarse sus moléculas forman una estructura cristalina más abierta y menos densa que el agua líquida."
    ),
    (
        ["¿Por qué vemos primero el relámpago y después oímos el trueno?",
         "¿Por qué el relámpago llega antes que el trueno?",
         "En una tormenta, ¿por qué se ve la luz del rayo antes de escuchar el trueno?",
         "¿Por qué escuchamos el trueno segundos después de ver el relámpago?"],
        "rayo emite luz y sonido juntos -> luz 300000 km/s vs sonido 340 m/s -> luz llega antes",
        "Porque la luz del relámpago viaja casi un millón de veces más rápido que las ondas sonoras del trueno a través del aire."
    ),
    (
        ["¿Qué pasa si calentamos agua líquida por encima de cien grados?", "¿Por qué si el agua hierve pasa a ser vapor?", "¿A qué temperatura hierve y se congela el agua?"],
        "nivel del mar -> congela a 0 C -> hierve a 100 C -> calor separa moleculas -> pasa a vapor",
        "A nivel del mar el agua se congela a cero grados Celsius y al llegar a cien grados Celsius hierve, separando sus moléculas para convertirse en vapor."
    ),
    (
        ["¿Qué pasaría en la Tierra si desaparecieran todas las plantas?", "¿Por qué son indispensables las plantas para los animales?"],
        "plantas hacen fotosintesis -> producen oxigeno y alimento base -> sin plantas falta oxigeno y colapsa cadena alimentaria",
        "Si desaparecieran las plantas caería la producción de oxígeno por fotosíntesis y colapsaría la base de toda la cadena alimentaria terrestre."
    ),
    (
        ["¿Qué es la fotosíntesis?", "Explícame cómo fabrican su alimento las plantas."],
        "luz solar + agua + dioxido de carbono -> hoja transforma en azucares -> libera oxigeno",
        "La fotosíntesis es el proceso mediante el cual las plantas usan la luz solar para transformar agua y dióxido de carbono en nutrientes, liberando oxígeno."
    ),
    (
        ["¿Por qué existen el día y la noche?", "¿Qué causa la sucesión del día y la noche en la Tierra?"],
        "Tierra rota sobre su eje cada 24 horas -> mitad iluminada por el Sol es dia -> mitad en sombra es noche",
        "El día y la noche existen porque la Tierra gira sobre su propio eje cada veinticuatro horas, iluminándose una mitad frente al Sol mientras la otra queda en sombra."
    ),
    (
        ["¿Por qué llueve?", "¿Cómo se forman las nubes y la lluvia?"],
        "calor evapora agua -> vapor sube y se enfria -> condensa en gotas -> caen por gravedad como lluvia",
        "Llueve porque el vapor de agua asciende, se enfría y se condensa en las nubes formando gotas cada vez más pesadas que caen por gravedad."
    ),

    # 3. Razonamiento deductivo, logico y comparativo
    (
        ["Si todos los mamíferos respiran aire y la ballena es un mamífero, ¿cómo respira la ballena?",
         "La ballena vive en el mar pero es un mamífero: ¿respira agua o aire?",
         "Todos los mamíferos respiran aire con pulmones y la ballena es mamífero. ¿Qué conclusión se deduce?",
         "Sabiendo que todo mamífero tiene pulmones y la ballena lo es, ¿qué necesita respirar?"],
        "silogismo mamiferos + pulmones -> ballena es mamifero -> ballena respira aire",
        "Por deducción lógica, como todos los mamíferos respiran aire con pulmones y la ballena es un mamífero, la ballena sale a la superficie para respirar aire."
    ),
    (
        ["Si Ana es mayor que Luis y Luis es mayor que Pedro, ¿quién es el menor de los tres?",
         "Ana tiene más años que Luis y Luis más que Pedro. ¿Quién es mayor y quién es menor?",
         "Sabiendo que Ana supera en edad a Luis y Luis supera a Pedro, ¿quién tiene menos edad?",
         "Si Ana es mayor que Luis y Luis mayor que Pedro, deduce el orden de edad."],
        "orden transitivo: Ana > Luis > Pedro -> Ana es la mayor y Pedro es el menor",
        "Siguiendo el orden transitivo, Ana es la mayor de los tres y Pedro es necesariamente el menor."
    ),
    (
        ["¿Qué pesa más, un kilo de hierro o un kilo de algodón?",
         "Compara el peso de un kilogramo de hierro y un kilogramo de algodón.",
         "¿Cuál tiene mayor masa: un kilo de hierro o un kilo de algodón?",
         "Entre un kilo de algodón y un kilo de hierro, ¿alguno pesa más?"],
        "hierro = 1 kg y algodon = 1 kg -> masas iguales -> pesan exactamente igual",
        "Ambos pesan exactamente lo mismo: un kilogramo; la única diferencia es que el algodón ocupa mucho más volumen por ser menos denso."
    ),
    (
        ["¿Qué diferencia hay entre una estrella y un planeta?",
         "¿En qué se diferencia el Sol de la Tierra?",
         "Explica la diferencia entre un planeta y una estrella."],
        "estrella como el Sol -> fusion nuclear y luz propia | planeta -> orbita y refleja luz",
        "Una estrella como el Sol genera luz y calor propios por fusión nuclear, mientras que un planeta gira alrededor de una estrella y solo refleja su luz."
    ),
    (
        ["¿Cuáles son los planetas del sistema solar?",
         "Dime en orden los ocho planetas que giran alrededor del Sol.",
         "Enumera los planetas que forman el sistema solar.",
         "¿Qué ocho planetas giran en torno al Sol?"],
        "sistema solar en orden -> Mercurio Venus Tierra Marte Jupiter Saturno Urano Neptuno",
        "Los ocho planetas en orden desde el Sol son Mercurio, Venus, la Tierra, Marte, Júpiter, Saturno, Urano y Neptuno."
    ),

    # 4. Razonamiento aritmetico y cuantitativo
    (
        ["¿Cuánto es doce más ocho?", "Suma doce más ocho.", "Calcula la suma de doce y ocho."],
        "suma aritmetica -> 12 + 8 = 20",
        "Doce más ocho es igual a veinte."
    ),
    (
        ["¿Cuánto es siete por seis?", "Multiplica siete por seis.", "¿Cuál es el producto de siete por seis?"],
        "multiplicacion -> 7 x 6 = 42",
        "Siete multiplicado por seis es cuarenta y dos."
    ),
    (
        ["Si tengo tres cajas con cinco libros en cada una, ¿cuántos libros tengo en total?",
         "Hay tres cajas y cada caja guarda cinco libros. ¿Cuántos libros hay?",
         "Calcula cuántos libros suman tres cajas con cinco libros por caja.",
         "Con tres cajas de cinco libros cada una, ¿qué total de libros resulta?"],
        "3 cajas x 5 libros por caja -> 3 * 5 = 15 libros",
        "Multiplicando tres cajas por cinco libros en cada una, tienes quince libros en total."
    ),
    (
        ["Si tenías veinte manzanas y regalas siete, ¿cuántas te quedan?", "¿Cuánto es veinte menos siete?"],
        "cantidad inicial 20 -> restar 7 regaladas -> 20 - 7 = 13",
        "Al restar siete de las veinte iniciales, quedan trece."
    ),

    # 5. Literatura, ciencia, lengua y salud
    (
        ["¿Quién escribió Don Quijote de la Mancha?", "¿Quién escribió el Quijote?", "¿Quién fue el autor de Don Quijote?"],
        "obra Don Quijote -> autor Miguel de Cervantes Saavedra -> publicado en 1605 y 1615",
        "Don Quijote de la Mancha fue escrito por Miguel de Cervantes Saavedra y publicado en dos partes, en 1605 y en 1615."
    ),
    (
        ["¿Cómo se llama el compañero fiel de don Quijote?", "¿Quién es Sancho Panza?", "¿Cómo se llama el escudero de don Quijote?"],
        "personajes del Quijote -> escudero fiel y practico -> Sancho Panza",
        "Sancho Panza es el fiel escudero y compañero de aventuras de don Quijote, célebre por su lealtad y su sentido práctico."
    ),
    (
        ["¿Quién fue Benito Pérez Galdós?", "Háblame de Benito Pérez Galdós y sus novelas."],
        "literatura siglo XIX -> Benito Perez Galdos -> Marianela, Tormento, Nazarin, Fortunata y Jacinta",
        "Benito Pérez Galdós fue un gran novelista realista español del siglo diecinueve, autor de Marianela, Tormento, Nazarín y Fortunata y Jacinta."
    ),
    (
        ["¿Quién fue Gustavo Adolfo Bécquer?", "Háblame de Gustavo Adolfo Bécquer."],
        "poesia romantica espanola -> Gustavo Adolfo Becquer -> Rimas y Leyendas",
        "Gustavo Adolfo Bécquer fue un poeta y narrador español del siglo diecinueve, célebre por sus Rimas y sus Leyendas."
    ),
    (
        ["¿Qué es la ciencia?", "Define qué es la ciencia con claridad.", "¿Qué es exactamente la ciencia?"],
        "ciencia -> observacion + hipotesis + experimentos reproducibles -> conocimiento verificable",
        "La ciencia es la búsqueda rigurosa de conocimiento verificable mediante la observación, el razonamiento lógico y la experimentación reproducible."
    ),
    (
        ["¿Para qué sirve dormir bien?", "¿Por qué el cerebro necesita dormir cada noche?"],
        "sueno nocturno -> reactivacion hipocampo-corteza -> consolidar memoria y recuperar el cuerpo",
        "Dormir bien permite descansar el organismo y consolidar en la corteza cerebral los recuerdos y aprendizajes del día."
    ),
    (
        ["¿Cuál es la capital de España?", "¿En qué país y continente está Madrid?"],
        "geografia -> Espana en peninsula iberica, Europa -> capital Madrid",
        "La capital de España es Madrid, situada en el centro de la península ibérica, en Europa."
    ),
    (
        ["¿Qué idioma hablamos?", "¿En qué lengua estamos conversando?"],
        "idioma actual -> espanol o castellano -> lengua romance",
        "Estamos conversando en español, también llamado castellano, una lengua romance precisa y expresiva."
    ),
    (
        ["¿Cuál es la capital de Venezuela y dónde se ubica?",
         "Háblame de Caracas, la capital de Venezuela.",
         "¿Cuál es la capital de Venezuela?",
         "¿Cuál es la capital de Venezuela y qué mar o cerro tiene cerca?",
         "¿Cómo se llama la capital de Venezuela y junto a qué montaña o cordillera está?"],
        "Venezuela en Sudamerica -> valle junto al cerro El Avila -> capital Caracas",
        "Caracas es la capital de Venezuela, situada en un valle al norte del país, al pie del cerro El Ávila y cerca del mar Caribe."
    ),
    (
        ["¿Cuál es la cordillera más larga de América del Sur y cuál es su río más caudaloso?",
         "Háblame de los Andes y del río Amazonas en Sudamérica.",
         "¿Qué gran cordillera y qué gran río destacan en América del Sur?"],
        "Sudamerica -> cordillera de los Andes al oeste + rio Amazonas el mas caudaloso",
        "En América del Sur destacan la cordillera de los Andes, que recorre el oeste del continente, y el río Amazonas, el más caudaloso del planeta."
    ),
    (
        ["¿Qué función cumple el corazón en el cuerpo humano?",
         "¿Para qué sirve el corazón?",
         "Explica cómo trabaja el corazón en la circulación sanguínea.",
         "¿Qué hace el corazón en nuestro organismo?"],
        "corazon humano -> musculo que impulsa la sangre -> reparte oxigeno y nutrientes",
        "El corazón impulsa rítmicamente la sangre por todo el cuerpo para llevar oxígeno y nutrientes a cada célula."
    ),
    (
        ["¿Qué es el ADN y para qué sirve?",
         "Explica qué función tiene el ADN en los seres vivos.",
         "¿Qué almacena la molécula de ADN?",
         "¿Qué guarda el ADN dentro del núcleo celular?"],
        "ADN en el nucleo celular -> doble helice -> instrucciones geneticas hereditarias",
        "El ADN es una molécula en doble hélice que guarda en el núcleo celular las instrucciones genéticas hereditarias de los seres vivos."
    ),
    (
        ["¿Qué es la gravedad y por qué caen los objetos al suelo?",
         "¿Por qué la Luna no se escapa de la Tierra?",
         "Explica cómo actúa la fuerza de gravedad.",
         "¿Por qué caen las cosas al suelo y además los astros giran en órbita?"],
        "fuerza de gravedad -> atraccion entre masas -> caida en la Tierra y orbitas espaciales",
        "La gravedad es la atracción universal entre cuerpos con masa; hace caer los objetos hacia la Tierra y mantiene en órbita a la Luna y los planetas."
    ),
    (
        ["¿Por qué fue tan importante la invención de la imprenta?",
         "¿Qué cambió en la historia humana gracias a la imprenta de Gutenberg?",
         "Explica el impacto histórico de la imprenta.",
         "¿Qué impacto tuvo la imprenta de Gutenberg?"],
        "imprenta de Gutenberg -> reproducir libros rapido -> difundir lectura y ciencia",
        "La imprenta de Gutenberg permitió reproducir libros con rapidez, multiplicando el acceso a la lectura, la educación y la ciencia."
    ),
    (
        ["¿Por qué estás programado en C++?",
         "¿Qué ventaja ofrece C++ para simular neuronas pulsantes?",
         "¿Por qué usar C++ en motores neuronales?",
         "¿Por qué es ventajoso C++ para tus neuronas LIF?"],
        "lenguaje C++ -> memoria contigua SoA y grafos CSR -> velocidad nativa en CPU",
        "En C++ gestiono arreglos contiguos y grafos dispersos CSR directamente sobre el procesador con velocidad nativa y mínima memoria."
    ),
    (
        ["Si hoy llueve el suelo se moja; hoy está lloviendo, ¿cómo está el suelo?",
         "Sabiendo que siempre que llueve se moja la calle y ahora llueve, ¿qué ocurre?",
         "Aplica modus ponens: si llueve se moja el suelo, y hoy llueve.",
         "Cuando llueve se moja la acera y ahora está lloviendo, ¿cómo quedará?",
         "Si siempre que llueve se moja el pavimento y hoy llueve, ¿cómo estará el pavimento?",
         "Si cuando llueve se empapa la vereda y hoy está lloviendo, ¿cómo quedará?"],
        "regla modus ponens -> si llueve se moja + hoy llueve -> el suelo esta mojado",
        "Por la regla deductiva de modus ponens, si cuando llueve se moja la superficie y ahora está lloviendo, necesariamente está mojada."
    ),
]

# Episodios multi-turno: (p1, pensa1, r1, lista_p2_ambiguas, pensa2_contextual, r2)
DIALOGOS_MULTITURNO = [
    (
        "¿Quién escribió Don Quijote de la Mancha?",
        "obra Don Quijote -> autor Miguel de Cervantes",
        "Don Quijote de la Mancha fue escrito por Miguel de Cervantes Saavedra y publicado en 1605 y 1615.",
        ["¿Y cómo se llama el escudero en esa novela?",
         "¿Y quién acompaña al protagonista en esa obra?",
         "¿Y cómo se llama su fiel escudero en esa historia?"],
        "novela Quijote de Cervantes -> escudero del caballero -> Sancho Panza",
        "En esa novela de Cervantes, el fiel escudero que acompaña al caballero se llama Sancho Panza."
    ),
    (
        "¿Qué es la fotosíntesis?",
        "plantas + luz solar -> azucares + oxigeno",
        "La fotosíntesis es el proceso mediante el cual las plantas usan la luz solar para fabricar nutrientes y liberar oxígeno.",
        ["¿Y qué gas liberan a la atmósfera durante ese proceso?", "¿Y qué producen para que podamos respirar?"],
        "proceso de fotosintesis vegetal -> gas liberado al aire -> oxigeno",
        "Durante ese proceso de fotosíntesis, las plantas liberan oxígeno a la atmósfera."
    ),
    (
        "¿A qué temperatura se congela el agua?",
        "agua pura a nivel del mar -> congela a 0 grados Celsius",
        "A nivel del mar, el agua pura se congela a cero grados Celsius.",
        ["¿Y por qué flota cuando pasa a ese estado sólido?", "¿Y por qué no se hunde entonces?"],
        "hielo solido a 0 C -> red cristalina abierta menos densa -> flota",
        "Cuando el agua pasa a ese estado sólido forma hielo cristalino menos denso que el agua líquida, y por eso flota."
    ),
    (
        "¿Quién fue Benito Pérez Galdós?",
        "novelista espanol siglo XIX -> Galdos",
        "Benito Pérez Galdós fue uno de los grandes novelistas realistas españoles del siglo diecinueve.",
        ["¿Y qué novelas famosas escribió ese autor?", "¿Puedes nombrar obras suyas?"],
        "novelas de Benito Perez Galdos -> Marianela, Tormento y Nazarin",
        "Entre las novelas más célebres de Benito Pérez Galdós destacan Marianela, Tormento, Nazarín y Fortunata y Jacinta."
    ),
    (
        "Si tengo tres cajas con cinco libros en cada una, ¿cuántos libros tengo?",
        "3 cajas x 5 libros -> 15 libros",
        "Multiplicando tres cajas por cinco libros en cada una, tienes quince libros en total.",
        ["¿Y si regalo cinco de esos libros cuántos me quedan?", "¿Y si presto cinco cuántos quedan?"],
        "quince libros acumulados -> restar cinco entregados -> 15 - 5 = 10 libros",
        "Si tenías quince libros del cálculo anterior y entregas cinco, te quedan exactamente diez libros."
    ),
    (
        "¿Qué eres?",
        "identidad -> CORTEX-LM con neuronas LIF sin backprop",
        "Soy CORTEX-LM, un cerebro de lenguaje en español con neuronas LIF puras y grafos CSR en C++, sin retropropagación.",
        ["¿Y cómo logras recordar lo que hablamos en el turno anterior?", "¿Y cómo mantienes el hilo de esta conversación?"],
        "hilo de dialogo en CORTEX-LM -> trazas lentas NMDA L2/L3 retenidas",
        "Mantengo el hilo entre turnos conservando activas mis trazas sinápticas lentas NMDA en las regiones corticales superiores."
    ),
    (
        "¿Cuál es la capital de Venezuela?",
        "Venezuela en Sudamerica -> valle junto al cerro El Avila -> capital Caracas",
        "Caracas es la capital de Venezuela, situada en un valle al norte del país, al pie del cerro El Ávila y cerca del mar Caribe.",
        ["¿Y qué caída de agua famosa tiene ese país?",
         "¿Y cuál es el salto de agua más alto del mundo que está allí?",
         "¿Y qué gran salto de agua famoso hay en ese país?"],
        "territorio de Venezuela -> catarata mas alta del planeta -> Salto Angel",
        "En territorio venezolano se encuentra el Salto Ángel, la caída de agua más alta del mundo."
    ),
    (
        "¿Para qué sirve el corazón?",
        "corazon humano -> musculo que impulsa la sangre -> reparte oxigeno y nutrientes",
        "El corazón impulsa rítmicamente la sangre por todo el cuerpo para llevar oxígeno y nutrientes a cada célula.",
        ["¿Y qué molécula guarda las instrucciones genéticas dentro de esas células?",
         "¿Y dónde está escrita la herencia dentro de ellas?",
         "¿Y qué molécula hereditaria hay en el núcleo de esas células?"],
        "nucleo de esas celulas -> herencia genetica -> doble helice ADN",
        "Dentro del núcleo de esas células, el ADN guarda en su doble hélice todas las instrucciones genéticas."
    ),
]


def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    data_dir = os.path.join(base_dir, "data")
    os.makedirs(data_dir, exist_ok=True)

    bloques = []
    n_delib = 0
    n_direct = 0
    n_multi = 0

    for preguntas, pensa, resp in GRUPOS_RAZONAMIENTO:
        for idx, p in enumerate(preguntas):
            # Todos los episodios se entrenan en modo deliberativo (~ pensamiento -> > respuesta)
            bloques.append(f"{p}\n~ {pensa}\n> {resp}\n")
            n_delib += 1
            if idx == 0:
                bloques.append(f"{p}\n> {resp}\n")
                n_direct += 1

    for p1, pen1, r1, lista_p2, pen2, r2 in DIALOGOS_MULTITURNO:
        for p2 in lista_p2:
            bloques.append(f"{p1}\n~ {pen1}\n> {r1}\n---\n{p2}\n~ {pen2}\n> {r2}\n")
            n_multi += 1

    texto_ali = "\n".join(bloques) + "\n"

    # Para conocimiento_es.txt usamos prosa limpia + Q&A limpio (sin marcadores internos [[...]])
    qa_limpio = [f"{preguntas[0]}\n{resp}" for preguntas, _, resp in GRUPOS_RAZONAMIENTO]
    texto_conocimiento = EXPOSICION + "\n\n" + "\n\n".join(qa_limpio) + "\n"

    ruta_con = os.path.join(data_dir, "conocimiento_es.txt")
    ruta_ali = os.path.join(data_dir, "alineacion_es.txt")
    with open(ruta_con, "w", encoding="utf-8") as f:
        f.write(texto_conocimiento)
    with open(ruta_ali, "w", encoding="utf-8") as f:
        f.write(texto_ali)

    ruta_dic = os.path.join(data_dir, "dic_amplio.txt")
    if os.path.exists(ruta_dic):
        with open(ruta_dic, encoding="utf-8") as f:
            palabras = set(ln.strip() for ln in f if ln.strip())
        for m in re.finditer(r"[a-záéíóúüñ]{2,}", texto_conocimiento + "\n" + texto_ali, flags=re.I):
            palabras.add(m.group(0).lower())
        with open(ruta_dic, "w", encoding="utf-8") as f:
            f.write("\n".join(sorted(palabras)) + "\n")

    print(f"Generado {ruta_con} ({len(texto_conocimiento):,} chars)")
    print(f"Generado {ruta_ali} ({n_delib} deliberativos + {n_direct} directos + {n_multi} multi-turno = {len(bloques)} episodios, {len(texto_ali):,} chars)")


if __name__ == "__main__":
    main()
