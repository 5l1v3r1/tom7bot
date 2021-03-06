structure FSUtil =
struct

    exception FSUtil of string

    fun splitext s =
        StringUtil.rfield (StringUtil.ischar #".") s

    fun chdir_excursion s f =
        let val {arcs, isAbs, vol} = OS.Path.fromString s
        in
            (case rev arcs of
                 nil => raise FSUtil "no file in chdir_excursion"
               (* don't need to move *)
               | [file] => 
                     let in
                         (* print ("just: " ^  file ^ "\n"); *)
                         f file
                     end
               (* move to dir and come back *)
               | (file::rest) =>
                 let
                     val new = OS.Path.toString 
                                {arcs = rev rest, isAbs=isAbs, vol=vol}
                     val old = OS.FileSys.getDir ()
                 in
                     (* print ("old: " ^ old ^ "; new: " ^ new ^ "\n"); *)
                     let in
                         OS.FileSys.chDir new;
                         f file before
                         OS.FileSys.chDir old
                     end handle ex => (OS.FileSys.chDir old; raise ex)
                 end)

        end

  (* assume exists = can open. This is probably not the best implementation.. *)
  fun exists f =
      (BinIO.closeIn (BinIO.openIn f); true) handle _ => false

  fun tempfilename base =
      let
          fun tf n =
              let val f = base ^ Int.toString n ^ ".tmp"
              in
                  if exists f
                  then tf (n + 1)
                  else f
              end
      in
          tf 0
      end

(*
  fun dirplus "" p = p
    | dirplus d p =
      case CharVector.sub(d, size d - 1) of
          #"/" => d ^ p
        | _    => d ^ "/" ^ p
*)
    fun dirplus d p = OS.Path.concat (d, p)
end
