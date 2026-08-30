# Politica de ramas del fork PPSSPP (proyecto STV)

Archivo **no versionado** a proposito: el fork no lleva documentacion propia
commiteada, para que la pila de parches quede limpia y cherry-pickeable hacia
upstream. La copia canonica de esta politica vive en el repo principal
(`docs/tareas/f13-rendimiento-experiencia.md`, tanda D).

## 1. La lib shippeada esta pineada en v1.20.4

La biblioteca que se hornea en la imagen sale SIEMPRE de `stv/gpu-escala`, que
parte del tag `v1.20.4` (commit de infra `STV infra: pin de version a v1.20.4`).
El pin no se mueve porque un salto de version de PPSSPP invalida de golpe todo
el banco de medicion (fps de referencia, escenas, CV) y los caches de shaders.

## 2. La pila de `stv/gpu-escala` NO se rebasea

Los 20+ commits propios sobre el pin son la unidad de auditoria del proyecto:
cada uno tiene su medicion asociada en `docs/tareas/` y su sha citado en la
memoria del agente. Un rebase reescribe todos los shas y rompe esa trazabilidad,
ademas de arriesgar resoluciones silenciosas de conflicto en codigo caliente
(GE worker, FramebufferManagerCommon, TextureCacheCommon).

Prohibido: `rebase`, `commit --amend` sobre commits ya horneados, `push --force`.
Permitido: commits nuevos encima.

## 3. Upstream se consume por cherry-pick, nunca por merge

Si un fix de upstream hace falta en la lib, se trae con
`git cherry-pick -x <sha>` sobre `stv/gpu-escala` y se documenta en la tarea que
lo motivo. Nada de `git merge upstream/master`: traeria el salto de version que
el punto 1 prohibe.

## 4. Los PRs a upstream salen de topic branches sobre `master`

`master` es una rama limpia que sigue a `upstream/master` por fast-forward
(remote `upstream` = https://github.com/hrydgard/ppsspp.git, `origin` =
el fork personal). Para cada PR:

    git fetch upstream master
    git fetch . upstream/master:master        # FF, falla si divergio
    git checkout -b upstream-pr/<slug> master
    git cherry-pick <sha del parche STV>      # o portar a mano
    # reescribir comentarios a ingles tecnico, sin marcas STV_*

Reglas del topic branch:

- Diff minimo: solo el fix, sin renombres ni reformateos.
- Comentarios y mensaje de commit en **ingles**; nada de `STV_*`, nombres de la
  consola ni referencias al proyecto interno.
- Un fix por rama, un PR por rama.
- No se abre PR de un parche entretejido con codigo propio (ej.: la escotilla
  del epilogo, que depende de `StvEpilogo.h`): esos se quedan locales.
- El trailer `Co-Authored-By` se decide antes de abrir el PR, no despues.

Ramas topic vigentes:

| rama | parche origen | destino |
|---|---|---|
| `upstream-pr/rotation-fix` | parche 7 (`db0df8394`) | PR |
| `upstream-pr/reload-pergame-guard` | parche 11 (`9470192b1`) | issue + PR |

## 5. Nada de `push` sin pedirlo

Vale tambien para las ramas topic. El usuario decide cuando y con que cuenta se
publican, y si se abre el PR o el issue primero.
